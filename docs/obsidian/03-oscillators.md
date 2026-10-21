# Osciladores

Hay 3 por voz (`Osc0..Osc2`; en la UI y la matriz se llaman Osc 1..3). Los parámetros
comunes están en [02-preset-format.md](02-preset-format.md#osc02--csynthosc-parámetros-comunes).
Abajo, cada tipo.

Uso en los presets de fábrica (Osc0 / Osc1 / Osc2) **[P]**:

| Tipo | Osc0 | Osc1 | Osc2 |
|---|---|---|---|
| Analog | 113 | 150 | 198 |
| Wavetable | 59 | 49 | 34 |
| FM | 55 | 37 | 13 |
| NanoSaw | 28 | 34 | 39 |
| PD | 23 | 11 | 7 |
| Noise | 21 | 27 | 18 |
| Sample | 13 | 4 | 3 |

Si hay que recortar para hardware chico, Analog + Wavetable + NanoSaw cubren la mayoría.

---

## Parte común: pitch y reparto por tipo **[B]**

Decompilado de `cSynthOsc` (`0x1005812e0`). El patch del oscilador guarda los floats en
el orden del XML: `[4] En [5] Type [6] Trn [7] Dtn [8] KeyT [9] Lev [10] Pan [11] FMix
[12] Ring [13] Widt [14] POff`.

```text
pitch = 60 + (nota − 60)·KeyT + Trn + Dtn + mod_pitch    // semitonos
pitch = clamp(pitch, 0, 127)
f     = 440 · 2^((pitch − 69)/12)
level = clamp(Lev + 2·mod_level, 0, 2) · (ganancia de velocity)
```

- `KeyT` pivota en **C4 (MIDI 60)**: con `KeyT = 0` todas las notas suenan como C4.
- `Dtn` está en **semitonos**: ±0.15 = ±15 cents.
- La modulación de pitch llega en semitonos; la escala `Dept → semitonos` la aplica la
  matriz ([06-modulation.md](06-modulation.md#escalas)).
- Vector de modulación del oscilador: `[0]` pitch, `[1]` level, `[2]`/`[4]` pan y filter
  mix, `[3]` fase, `[5]` ring, `[6]` width, y después los destinos propios de cada tipo.
- Glide: el pitch se acerca al destino con una pendiente máxima proporcional a
  `100·Δt`, en semitonos **[B]**. Cómo depende de `GTim` **[?]**.

Reparto por `Type` (switch en el mismo orden que el manual):

| Type | Función | Patch |
|---|---|---|
| 0 Analog | `0x100562188` | `+0x40` |
| 1 Wavetable | `0x10058cb60` | `+0x48` |
| 2 PD | `0x10056da2c` | `+0x50` |
| 3 FM | `0x100565478` | `+0x58` |
| 4 NanoSaw | `0x10046d1c4` | `+0x60` |
| 5 Noise | `0x10056c920` | `+0x68` |
| 6 Sample | `0x10056fddc` | `+0x70` |

## Analog (`Anlg` → `cOscTypeAnalogue`)

"Oscilador de modelado analógico con 6 formas de onda, PWM y sync" **[M]**.

| Attr | Interno | Valores |
|---|---|---|
| `Wave` | Waveform | Índice interno: 0 `Sine`, 1 `Tri`, 2 `Saw`, 3 `Square`, 4 `Pulse`, 5 `Bi-Pulse` **[B][I]** |
| `Sync` | SyncMode | `OFF`, `ANALOG`, `DIGITAL`; versiones viejas usan `Off`/`On` |
| `PWM` | PulseWidth | 0.01..0.5 |
| `SRat` | SyncRatio | 0..1 normalizado; formato UI `cUIParamFormatSyncRatio` |
| `SShp` | SyncSharpness | 0..1; en la matriz aparece como "An Sync Hardness" |

**Cómo funciona [B]** (`0x100562188`):

- **Sine, Tri, Saw y Square no son generadores separados**: son posiciones de una
  **wavetable de banda limitada**, `WaveTables/Analog.tbl`, con `posición = wave/3`. Los 4
  frames de ese archivo son exactamente seno, triángulo, sierra y cuadrada (verificado
  por su contenido armónico: solo fundamental; impares en 1/n²; todos en 1/n; impares en
  1/n). Se tocan con el **mismo motor de wavetable** del oscilador Wavetable
  (`0x1004e2234`).
- Los 4 frames de `Analog.tbl` tienen el mismo RMS, el de un seno de pico 1 (0.7071): la
  sierra y el triángulo pican a 1.22 y la cuadrada a 0.71 **[B]**. Los renders de NS2 lo
  confirman: saw, square, sine y tri suenan al mismo nivel **[P]**.
- **Fase inicial [P]**: con `POff` 0, la sierra arranca en 0 y **sube** (salta a −pico a
  media fase), el seno y el triángulo arrancan en 0 subiendo y la cuadrada arranca en +.
  La Pulse arranca en su tramo positivo angosto. Medido en el primer ciclo de cada nota de
  los renders de NS2. Con la sierra invertida, un LP muy cerrado da un golpe de ataque
  distinto, y cambian la suma con otros osciladores y la FM.
- Por lo tanto **el antialiasing son los mipmaps** de la tabla (44 niveles, 4 por octava,
  ver [08-wavetable-format.md](08-wavetable-format.md)). No hay BLEP.
- **Pulse (4)** y **Bi-Pulse (5)** usan `pw = clamp(PWM + mod, …)` sobre el mismo motor,
  con constantes propias (Pulse: 0.58 y una ganancia de 2/3; Bi-Pulse: 0.5 y 1). Lo más
  probable es que salgan como resta de dos lecturas de sierra desfasadas en `pw` **[I]**.
- **Sync** (interno al oscilador, no hay sync entre osciladores):
  `ratio = 2^(4 · clamp(SRat + mod, 0, 1))`, o sea de 1× a 16×.
  En uno de los dos modos activos se usa `SShp` (+mod) para calcular, con `exp2`, un
  entero de "dureza", probablemente el ancho de la ventana de suavizado; en el otro la
  dureza queda fija. Que el primero sea `ANALOG` y el segundo `DIGITAL` es la lectura
  natural **[I]**.
- `On` en presets viejos: mapearlo a `ANALOG` es la opción razonable **[?]**.

**Para un port**: un oscilador de wavetable con mipmaps resuelve Analog y Wavetable a la
vez. Las tablas de banda limitada de seno, triángulo, sierra y cuadrada se pueden generar
por síntesis aditiva sin usar el archivo de NS2.

## Wavetable (`Wave` → `cOscTypeWaveTable`)

| Attr | Interno | Valores |
|---|---|---|
| `Mode` | Mode | `Single` `Crossfade` `Detune-2` `Detune-4` `Spread-4` |
| `Pos` | Position | 0..1, posición en la tabla (morph) |
| `Dtn` | Detune | 0..1, para los modos de voces múltiples |
| `WT/@Name` | — | `Categoría/Nombre`, por ejemplo `Simple/Sin-Tri-Saw`. Corresponde a `WaveTables/<Categoría>/<Nombre>.tbl` |

- `Single`: sin interpolar entre frames (salta al frame más cercano) **[I]**.
- `Crossfade`: interpolación lineal entre frames vecinos **[I]**. Es el modo más usado (288/312).
- `Detune-2` **[B]**: 2 voces a `f·(0.99994 − 0.0619·d²)` y `f·(1.00006 + 0.0619·d²)`, con
  `d = Dtn` (+mod) recortado a 0..1. A `d = 1` quedan ±6.2 %, cerca de ±1 semitono.
- `Detune-4` **[B]**: 4 voces en dos pares, `f·(1 ± 0.018·d')`, con `d'` una versión
  escalada de `Dtn`.
- `Spread-4` **[B]**: 4 voces en `pitch + d·k_i` semitonos (cuatro offsets distintos), y se
  ajusta la posición en la tabla.

Implementación decompilada en `0x10058cb60` **[B]**: el oscilador recibe el pitch en
semitonos relativos a A4 (`f = 440·2^(p/12)`) y un modo 0..4 en el orden del enum.

Tablas disponibles (25) **[P]**:
- `Analog.tbl`
- `Simple/`: Sin-Tri-Saw, Sin-Saw-Sqr, Sin-Sqr-Tri, Tri Vintage, Saw Vintage, Sqr Vintage,
  Saw Octaves, Resonant, Highpassed
- `Complex/`: Shimmer, Digitalis, Buzz, Chimes, Mineral, Hum, Tech, Zing
- `Instrument/`: Vocal, Organ, Brassy, Orchestral, EP, Bass, Bowed

Formato binario en [08-wavetable-format.md](08-wavetable-format.md).

## PD, phase distortion (`PD` → `cOscTypePD`)

"Da sonidos tipo pasabajos y filtro resonante sin necesidad de un filtro" **[M]**. Es
el estilo Casio CZ. Decompilado en `0x10056da2c` y sus rutinas **[B]**.

| Attr | Interno | Valores |
|---|---|---|
| `Wave` | Waveform | 0 `Saw`, 1 `Square`, 2 `Pulse`, 3 `Double Sine`, 4 `Saw Pulse`, 5 `Res Saw`, 6 `Res Fat Saw`, 7 `Res Tri`, 8 `Res Tri HP` |
| `PD` | PD | 0..1, cantidad de distorsión de fase (+ mod "PD Phase Dist") |

**Cómo funciona:**

- Fase en un acumulador entero de **32 bits** (`inc = f/sr · 2³²`) y salida leída de una
  tabla de coseno de 2048 puntos (constantes `2047.5/d`).
- Cada forma de onda calcula, una vez por bloque, los **puntos de quiebre** de la función
  de transferencia de fase, en punto fijo de 16 bits (`65535·d`), y las pendientes de
  cada tramo. Para la sierra la deformación es la clásica del CZ:
  `φ' = φ < d ? 0.5·φ/d : 0.5 + 0.5·(φ − d)/(1 − d)` y después `cos(2π·φ')`.
- **El `PD` pasa por una curva por forma de onda** antes de convertirse en quiebre. Para
  Saw: `a = PD < 0.25 ? 0.08·PD : 1.3067·PD + c`, después `x = a^(1/4)` y
  `d = 0.5 − 0.5·x` (de 0.5, que da un seno, hasta casi 0, que da la sierra). Las demás
  usan otro exponente, cada una con su tramo lineal inicial: Square `√`, Pulse `^0.3`,
  Double Sine `^0.4` y Saw Pulse `^0.15`.
- **Antialiasing por pitch**: entre 65 Hz y 8372 Hz (C9) un polinomio en la frecuencia
  **limita la cantidad máxima de distorsión**, así las notas agudas tienen menos armónicos.
  Además hay dos variantes de render y se elige una según la frecuencia (umbral en
  `16384·f/sr·… > 16`).
- **Formas resonantes (5..8)**: una senoidal a `r` veces la frecuencia, con
  `r = 1 + k·PD²`, ventaneada por una envolvente de sierra o triángulo que se reinicia en
  cada ciclo (el "filtro resonante" del CZ). `r` se limita para que la resonancia quede
  debajo de Nyquist: por encima de 130 Hz el límite es `(−5·f + c)/f`, y por debajo es 64. La fase de la ventana se corrige por
  forma: 0.3/r (Res Saw), 0.25/r (Res Fat Saw), `(ceil(r/2) − 0.25)/r` o 0.67/r (Res Tri y
  Res Tri HP).
- En las formas resonantes, cuando la fase se resetea en el note-on, las primeras muestras
  se rampean desde 0 para evitar clicks (un fade de `0.0004·sr` samples, unos 0.4 ms).
- La constante `c` de la curva de Saw sale de pedir continuidad en `PD = 0.25`:
  `0.08·0.25 = 1.3067·0.25 + c` da `c = −0.3067`, y entonces `a = 1` en `PD = 1` **[I]**.
- En presets, el uso de formas es Saw 895, Square 10, Saw Pulse 7, Res Fat Saw 6, Pulse 6,
  Res Tri HP 5, Res Saw 3, Double Sine 3 y Res Tri 1 **[P]**.

## FM (`FM` → `cOscTypeFM`)

Tiene 3 operadores. Los tres pueden ser portadoras; **solo el 1 y el 2 pueden ser
moduladores**, incluso de sí mismos (feedback) **[M]**.

### Operadores `FM/OP/O1..O3` (`cOperator`)

| Attr | Interno | Valores |
|---|---|---|
| `Wave` | Waveform | `Sine` `Saw Up` `Saw Down` `Square` `Triangle` `Parabola` `Half Wave` `Full Wave` `Sine 1+2` `Sine 1+3` `Sine 1+4` `Sine 1+5` `Rez 2` `Rez 3` `Rez 5` |
| `Rat` | Ratio | Entero. **ratio = Rat · 0.0001** **[B]** (10000 = 1:1). Con modulación se recorta a 0..32 |
| `FrqO` | FreqOffset | Hz, desafinación fija (curva UI `SigmoidCentreZero<10>`) |

Con `Rat=0` y solo `FrqO`, el operador funciona como LFO **[M]**.

### Conexiones `FM/Con/Oxy` (`cConnection`)

| Elemento | Conexión |
|---|---|
| `O11` | OP1 → OP1 (feedback) |
| `O12` | OP1 → OP2 |
| `O13` | OP1 → OP3 |
| `O1O` | OP1 → salida |
| `O22` | OP2 → OP2 (feedback) |
| `O23` | OP2 → OP3 |
| `O2O` | OP2 → salida |
| `O3O` | OP3 → salida |

Cada conexión tiene `Amt` (0..1, interno `Amount`) y **su propia envolvente**
`Env` con el mismo formato que [las envolventes de voz](05-envelopes-lfos.md) **[P][M]**.
Los nombres internos están en el binario como `OP1ToOP2`, `OP1Out`, etc. **[B]**.

Grafo implícito, con OP2→OP1 y OP3→cualquiera ausentes:

```
OP1 ⟲ ─► OP2 ⟲ ─► OP3 ─► out
  │        │        ▲
  └────────┼────────┘
  └─► out  └─► out
```

Orden de cálculo por sample: OP1, OP2, OP3 **[I]**. No hay ciclos salvo el
autofeedback, así que se puede calcular en un solo paso.

### Implementación **[B]** (`0x100565478`)

- **Motor en punto fijo**, al estilo DX: fases enteras de 32 bits (`inc = f/sr · 2³²`).
- Frecuencia de cada operador: `ratio · f_nota` más el offset fijo `FrqO` en Hz. Con
  modulación (`FM OPn Freq`), `ratio = clamp(Rat·0.0001 + 10·m, 0, 32)` **[B]**. La banda de la
  tabla se elige con `ratio · f_nota` (sin `FrqO`) llevada a 44.1 kHz.
- **Formas de onda en tablas de 256 muestras int16**, embebidas en el binario, con
  **versiones de banda limitada** elegidas por la frecuencia del operador (normalizada a
  44.1 kHz). Tabla de punteros por forma en `0x100c2e110`, con 15 formas. Cada puntero lleva a
  una lista de `{float max_hz, u32, u64 tabla}`, que termina en `FLT_MAX` y apunta a una tabla
  muda **[B]**. Las tablas son de 257 int16 (con muestra de guarda). El orden del motor no es
  el de la UI. Se identificó por los armónicos y la pendiente de cada tabla **[B][I]**:
  0 Sine, 1 Parabola (impares en 1/k³), 2 Triangle (1/k²), 3 Saw Up, 4 Saw Down, 5 Square,
  6 Half Wave, 7 Full Wave, 8..11 Sine 1+2..1+5, 12..14 Rez 2/3/5 (pico en el armónico 2, 3
  o 5). La última banda de las 14 formas no seno es la tabla del seno (≤ 18 kHz).
  [`firmware/tools/fmwaves2h.py`](../../firmware/tools/fmwaves2h.py) las extrae.

  | Forma | Cortes (Hz a 44.1 kHz) |
  |---|---|
  | Sine | una sola tabla hasta 18000; por encima, silencio |
  | Las otras 14 | 400, 800, 1000, 1200, 1600, 3000, 18000; por encima, silencio |

  Así una sierra o una cuadrada como operador no hacen aliasing en notas agudas.
- Cada una de las 8 conexiones tiene su envolvente, que se dispara, libera o redispara
  con la voz. La cantidad (abajo) se **convierte a entero** y se interpola muestra a muestra
  dentro del bloque. Además hay un limitador de **±6 unidades por bloque** sobre un valor
  guardado, cuyo uso exacto **[?]**.
- **Render por sample** (`0x1002bf5cc`, una de varias versiones especializadas según qué
  conexiones están activas):

  ```text
  o1 = T1[ φ1 + fb1·avg1 ]                   // lectura con interpolación lineal (16 bits)
  o2 = T2[ φ2 + fb2·avg2 + m12·o1' ]         // o1', o2': salidas de la muestra anterior
  o3 = T3[ φ3 + m13·o1' + m23·o2' ]
  avg1 = (o1 + avg1) >> 1;  avg2 = (o2 + avg2) >> 1   // feedback promediado, como el DX7
  out  = (a1·o1 + a2·o2 + a3·o3) · 2.4651e-9 // a·: niveles O1O, O2O, O3O
  ```

  Las salidas son int16 y las cantidades enteros, así que la modulación de fase es
  `cantidad × salida` en unidades de fase de 32 bits, con multiplicaciones de 32 bits que
  dan la vuelta. Índice de tabla `φ >> 24`; fracción `(φ >> 8) & 0xffff`. Los moduladores
  usan la salida de la muestra anterior: el código guarda `o1`/`o2` y los lee antes de
  actualizarlos (`0x1002bf644`).
- **Cantidad de cada conexión [B]** (`0x100565478`):

  ```text
  e   = 1 − Levl + Levl · envolvente     // Levl de la Env de la conexión = profundidad
  a   = clamp(e × escala_velocity × (Amt + mod), 0, 1.785)
  int = K · a²                      // ley cuadrática
  K   = 272625  (O11, O12, O13)     // moduladores desde OP1
        262140  (O22, O23)          // moduladores desde OP2
        16384   (O1O, O2O, O3O)     // niveles de salida, Q15: 16384 = 0.5
  ```

  Lo que se mide en NS2 contradice esta lectura: con `Levl = 0` (el valor de casi todas las
  conexiones de fábrica) **la envolvente actúa completa** **[P]**. House Pluck Bass, con
  `O3O` en decay 1.5 s y sustain 0, llega a silencio digital en 1.5 s con la tecla apretada.
  El port usa `e = Levl + (1 − Levl) · envolvente`. Con eso FM Stoned Keys, House Pluck
  Bass, Hang Drum y Ef Em Crystal Keys coinciden con NS2 en nivel, envolvente y armónicos.
  Tests con `O3O` en decay 0.6 s y sustain 0 confirman la fórmula con `Levl` 0.5 y 1
  (coinciden exacto) **[P]**. Con `Levl` 0, NS2 decae igual hasta 100 ms, pero después queda
  una cola de −35 a −40 dB que el port baja a −56. Puede ser el limitador de ±6 unidades por
  bloque o la cuantización de la cantidad entera **[?]**.

  Con `Amt = 1` una conexión moduladora desplaza la fase hasta ~2.08 ciclos, o sea un
  **índice de modulación de ~13 rad**, del orden del máximo de un DX7 **[I]** (esto asume
  que no hay un shift extra en el render). Con modulación se puede llegar a `a = 1.785`.
- El orden interno de las 8 conexiones es O11, O12, O13, O22, O23, O1O, O2O, O3O; en el XML
  aparecen en orden alfabético (O11, O12, O13, O1O, O22, O23, O2O, O3O) **[B]**.

## NanoSaw (`NS` → `cOscTypeNanoSaw`)

"Varias sierras en capas y desafinadas" **[M]**. Es un supersaw. Decompilado en
`0x10046d1c4` **[B]**.

| Attr | Interno | Rango observado | Qué hace **[B]** |
|---|---|---|---|
| `Mix` | Mix | 0.18..1 | Nivel de las sierras laterales frente a la central |
| `FlOf` | Falloff | 0..1 | Cuánto bajan las parejas más desafinadas |
| `Dtn` | Detune | 0..0.75 | Desafinación máxima, con curva por tramos |
| `Sprd` | Spread | 0.3..0.9 | Reparto de la desafinación entre las 3 parejas (no es estéreo) |

**Algoritmo:**

```text
d  = clamp(Dtn + mod_detune, 0, 1)
D  = 0.5 · (d < 0.6 ? d/6 : 2.5·d − 1.4)      // curva continua con quiebre en 0.6
s  = 2.25 − 2·Sprd²
a3 = D                     // pareja externa
a2 = D · 1.5^(−s)          // pareja media
a1 = D · 3^(−s)            // pareja interna
frecuencias: f, f·(1 ± a1), f·(1 ± a2), f·(1 ± a3)        // 7 sierras

m  = clamp(Mix + mod_mix, 0, 1)
S  = k · 1.1 · (0.75 + 0.25·FlOf) · m
ganancias: centro = k·(1 − m/2)
           interna = S·(1 − FlOf/18)
           media   = S·(1 − 2·FlOf/9)
           externa = S·(1 − FlOf/2)
```

- `k` es la normalización del método de antialiasing (inversa de `f/sr`, ver abajo).
- **Nivel medido [P]** (tests `zn01`–`zn05`): con esas ganancias el port salía 1.1 dB más
  fuerte con `Mix` 0 y 6.2 dB con `Mix` 1. En NS2 la sierra central suena a 0.88 y las seis
  laterales a 0.52 de lo que da la fórmula; con esos factores los 5 tests quedan a ±1 dB.
- **Antialiasing: DPW** (Differentiated Parabolic Wave). Cada sierra tiene fase entera
  de 32 bits, y el estado previo se inicializa como `(fase/2³¹)²`, la parábola que el
  método deriva **[B]**.
- **Fases iniciales aleatorias** en cada note-on (LCG `x·31415821 + 1`, el mismo de los
  LFOs). La sierra central arranca siempre en `0x7FFFFFFF` **[B]**.
- Internamente hay **dos grupos de 4 sierras**: centro + las tres desafinadas hacia abajo,
  y centro + las tres hacia arriba. Probablemente uno por canal, con `Widt` controlando la
  mezcla estéreo **[I]**.

## Noise (`Nois` → `cOscTypeNoise`)

| Attr | Interno | Valores |
|---|---|---|
| `Type` | Type | 0 `White`, 1 `Pink`, 2 `Brown`, 3 `Low Passed`, 4 `High Passed`, 5 `Band Passed`, 6 `Notched` |
| `Deci` | Decimate | 0..1, reducción de sample rate **que sigue la nota** |
| `Cut` | Cutoff | 0..1, solo tipos 3..6 |
| `Q` | Q | 0..1, solo tipos 3..6 |

**Implementación [B]** (`0x10056c920`, `0x10056cd20` mono y `0x10056ca30` estéreo):

- **Generador**: dos enteros de 32 bits, `x' = (x >> 2) + 4x + y` e `y' = x' + y`, y
  la salida se convierte a float. Es un generador aditivo barato, no el LCG de los LFOs.
- **Decimación (sample & hold)**: un acumulador de fase fraccionaria de 16 bits decide
  cuándo sacar una muestra nueva; mientras tanto repite la anterior. La tasa es

  ```text
  d    = clamp(Deci + mod, 0, 1)
  rate = 2^((pitch − 60 − 128·d) / 12)      // muestras nuevas por muestra, ≤ 1
  ```

  Con `Deci = 0` y la nota en C4 o más arriba no hay decimación; por debajo de C4 el ruido
  se decima en proporción a la nota, así que el ruido queda "afinado". `Deci = 1` baja la
  tasa 128 semitonos (unas 10.7 octavas).
- **Pink**: suma de 3 filtros de un polo sobre el ruido blanco (estilo "economy pink" de
  Paul Kellet) **[B][I]**.
- **Brown**: paseo aleatorio acotado; si el siguiente paso se sale de rango, se queda en
  el valor anterior **[B]**.
- **Low/High/Band Passed y Notched**: pasan por un filtro de la **familia biquad**
  (los mismos diseñadores que los filtros `-D`) **[B]**:

  ```text
  f = 20 · 1000^(Cut + mod)                  // 20 Hz .. 20 kHz
  q = 0.25 + k · (Q + mod)²                  // k = 15.75; en Notched, k = 3.75
  if f < 40 Hz: q *= 0.025 · f
  ```
- Con ancho estéreo (`Widt` > 0) se generan dos canales de ruido independientes con SIMD
  **[B][I]**.

## Sample (`Samp` → `Zones` → `Zone*`)

Hasta **24 zonas**, con como mucho 2 superpuestas (crossfade) **[M]**.

| Attr de `Zone` | Valores |
|---|---|
| `File` | Ruta. `/AppRes/...` = recurso de la app; nombre suelto = archivo en la carpeta del preset; `""` = vacío |
| `Root` | Nota MIDI raíz |
| `Lo` / `Hi` | Rango de notas MIDI |
| `Tune` | Afinación fina (±0.15 observado). El render usa `Root − Tune` como raíz efectiva **[B]**: sube la zona `Tune` semitonos **[I]** |
| `Vol` | Ganancia lineal |
| `Pan` | 0..1 |
| `Dir` | `Forward`, `Fwd-Rev` |
| `Loop` | `Off`, `On`, `Spectral` |

- **Spectral loop**: genera un sample nuevo a partir del espectro del original, para que
  se pueda loopear sin clicks **[M]**. El binario tiene ventanas (`Hanning`, `Gaussian`,
  `BlackmanH`, `Welch`…) y variantes `Spectral (Entire Sample)` / `Spectral (Sustain Loop)`
  **[B]**. Es el bloque más caro y el primer candidato a dejar afuera en un port.
- Los puntos de loop no están en la zona: vienen de los metadatos `smpl` del WAV (el
  string `Smpl` está en el binario) **[I]**. Los WAV de un preset de usuario con samples
  (RhodesMK2) traen un loop forward por archivo en `smpl`, con el final inclusivo **[P]**.
- Destinos de modulación propios: `Sample Start`, `Sample Loop Start`, `Sample Loop Len`,
  `Sample Zone Shift` **[B]**.

### Cómo suena **[B]**

Decompilado de `0x10056fddc` (note-on), `0x10056ff7c` (arranque de zona) y `0x10057020c`
(render).

- **Elección de zona**: la nota (más `Sample Zone Shift`·100), recortada a 0..127, se
  compara con `Lo..Hi` de cada zona en orden. Suenan **las dos primeras zonas** que la
  contienen, cada una en su propio reproductor **[B]**. Que el valor sea la tecla MIDI y no
  el pitch con `Trn` es **[I]**.
- **Crossfade entre zonas** solapadas: la ganancia de una zona se calcula contra la zona
  vecina con `(nota + 0.5 − Lo)/(Hi_vecina − Lo + 1)` (subiendo) o
  `(Hi + 0.5 − nota)/(Hi − Lo_vecina + 1)` (bajando), recortada a 0..1 **[B]**. Fuera
  del solape la ganancia es 1.
- **Velocidad de lectura**: `2^((pitch − (Root − Tune))/12)` por la relación entre el
  sample rate del archivo y el del motor, con el `pitch` del oscilador de la
  [parte común](#parte-común-pitch-y-reparto-por-tipo-b) **[B][I]**.
- **Ganancia**: crossfade de zona · `Vol` · paneo de la zona. El paneo es de potencia
  constante (`sin`/`cos` de `Pan·π/2`); con la zona mono al centro aplica `0.7071` **[B]**.
- `Dir` y el tipo de `Loop` eligen el modo del reproductor (`0x10059f774`); `Spectral`
  arranca sin dirección propia **[B]**. El detalle del reproductor (interpolación,
  `Fwd-Rev`) **[?]**.

Un preset de usuario confirma la convención de `Root` **[P]**: en RhodesMK2 los samples
suenan ~1.5 semitonos por debajo de su `Root` (autocorrelación del WAV) y el preset lo
compensa con `Trn` 1 y `Dtn` ≈ 0.47 en los tres osciladores. Con la fórmula de arriba, el
port toca `ROM C4` (Root 72) en la nota 72 a 522.9 Hz (C5 = 523.25 Hz).

---

## Capas y splits por velocity (`Osc` → `cOscTriple`) **[B]**

Decompilado de `0x1005a6178`, que llaman el note-on (`0x1005a60b8`) y el legato
(`0x1005a6450`, solo si cambió la velocity). Calcula una ganancia `g0..g2` por oscilador a
partir de la velocity `v` (0..1; que sea `vel/127` es **[I]**). Un oscilador con ganancia 0
no arranca ni se renderiza; la ganancia multiplica el nivel del oscilador. Un crossfade
con `LX < 0.01` es un corte seco.

`Type` es el enum `VelSplitType`: `Layer` (0), `Split 2` (1), `Split 3` (2). Los
parámetros están en [02](02-preset-format.md#osc--cosctriple-capas-y-splits-por-velocity).

```text
Layer:    g0 = 1
          gk = 1                         si v ≥ LLk          (k = 1, 2; Osc 2 y Osc 3)
             = (v − t)/(LLk − t)         si v > t,  t = LLk·(1 − LXk)
             = 0                         si no
Split 2:  g2 = 1                                             (Osc 3 no participa)
          v ≤ S2L1:  g0 = 1, g1 = 0
          si no:     e = S2L1 + LX1·(1 − S2L1)
                     v < e:  g0 = (e − v)/(e − S2L1), g1 = 1 − g0
                     si no:  g0 = 0, g1 = 1
Split 3:  v ≤ S3L1:         g = (1, 0, 0)
          v ≤ S3L2:         e = S3L1 + LX1·(S3L2 − S3L1)
                            v < e:  g0 = (e − v)/(e − S3L1), g1 = 1 − g0, g2 = 0
                            si no:  g = (0, 1, 0)
          v > S3L2:         e = S3L2 + LX2·(1 − S3L2)
                            v < e:  g2 = (v − S3L2)/(e − S3L2), g1 = 1 − g2, g0 = 0
                            si no:  g = (0, 0, 1)
```

- `LX1`/`LX2` son el **ancho del crossfade como fracción de la zona de arriba**: el
  crossfade entre Osc 1 y Osc 2 ocupa el principio de la zona media, y el de Osc 2 a
  Osc 3 el principio de la zona alta. La zona baja nunca mezcla.
- En `Layer`, `LL1`/`LL2` son la velocity desde la que entra Osc 2/Osc 3, con un fade-in
  de ancho `LX·LL` por debajo. Los defaults (`LL = 0`) hacen sonar siempre los tres.
- Ejemplo (RhodesMK2, `Split 3`, `S3L1` 0.353, `S3L2` 0.8745, `LX1` 1, `LX2` 0.0325):
  Osc 1 solo hasta 0.353; de 0.353 a 0.8745 crossfade lineal Osc 1 → Osc 2 (en v = 0.5,
  0.72/0.28); de 0.8745 a 0.8786 Osc 2 → Osc 3; arriba, Osc 3 solo.
