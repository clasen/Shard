# Envolventes y LFOs

## Envolventes (`cEnvelopeADSR`)

Hay 5 por voz **[M][P]**:

| Elemento | Nombre en la matriz | Conexión fija |
|---|---|---|
| `EnvA` | Amp Env | Nivel y duración de la voz (VCA). La voz termina cuando esta envolvente llega a 0 |
| `EnvF` | Filter Env | — |
| `EnvP` | Pitch Env | — (en 272 presets es `Bipolar`) |
| `Env4` | Env 4 | — |
| `Env5` | Env 5 | — |

Las conexiones FM (`FM/Con/Oxy/Env`) usan el mismo formato **[P]**.

### Tipos (`Type`)

- `ADSR`: delay → attack → decay → (breakpoint → decay2) → sustain → release **[M][B][I]**.
- `AD`: sin sustain, típico de percusión **[M]**. En los presets de fábrica de batería
  la amplitud AD tiene `S` 1 y `R` corto, o sea que el decay debe ir a 0 y no a `S`
  **[P][I]**. El port lo toca así e ignora el note-off **[I]**.
- `Bipolar`: la salida va de -1 a +1, típico para el pitch **[M]**. Usa niveles con signo en
  los extremos (`SLv`, `RLv`). Modelo del port **[P][I]**: arranca en `SLv`, el attack la
  lleva a 0, se queda en 0 (`D` y `S` no se usan) y el release va a `RLv`; todo por `Levl`.
  - La configuración por defecto (`SLv` 0, `S` 1, `Levl` 1, con `Pitch Env → Pitch Coarse,
    Dept 12`) está en 352 presets y tiene que ser neutra. Con "attack a 1 y sustain `S`" todos
    sonaban una octava arriba.
  - Los presets que la usan coinciden con este modelo: FM Electro Bass (`SLv` 1, attack de
    40 ms, un golpe de pitch de +12 que cae a 0) y Reese Bass 2 (`SLv` −1, sube desde −12).
  - Que `Levl` escale la salida: Hoover Vacuum (`SLv` 0.74, `Levl` 0.195, Dept 100) da una
    caída de 14 semitonos; sin escalar serían 74.
  - Falta confirmarlo en el binario (`0x10053163c`).

### Parámetros

| Attr | Interno | Rango observado | Formato UI **[B]** | Notas |
|---|---|---|---|---|
| `Dly` | Delay | 0..1 s | tiempo | Espera antes del ataque |
| `A` | Attack | 0..10 s | `cFormatTimeAttack` | |
| `D` | Decay | 0..20 s | tiempo | |
| `Brk` | BreakPoint | 0..1 | % | Nivel donde termina `D` y empieza `D2` **[I]** |
| `D2` | Decay2 | 0..2.5 s | tiempo | Segundo decay, de `Brk` a `S` **[I]** |
| `S` | Sustain | 0..1 | % | |
| `R` | Release | 0..8.5 s o `INF` | `cFormatTimeInf` | `INF` = la nota no se apaga sola |
| `Levl` | Level | 0..1 | % | Nivel de salida (pico) |
| `SLv` | BiAttackLevel | -1..1 | % con signo | Nivel inicial en modo Bipolar **[I]** |
| `RLv` | BiReleaseLevel | -1..1 | % con signo | Nivel final del release en Bipolar **[I]** |
| `AC` | AttackCurve | 1.03..33.3 | ver abajo | |
| `DC` | DecayCurve | 1.03..33.3 | | |
| `SC` | Decay2Curve **[I]** | casi siempre 1.22051 | | |
| `RC` | ReleaseCurve | 1.06..9.6 | | |
| `VLev` | VelLevel | -1..1 | % con signo | Cuánto escala la velocity el nivel |
| `VTim` | VelTime | -1..1 | % con signo | Cuánto escala la velocity los tiempos |
| `KCtr` | KeyCentre | nota MIDI (12..115) | entero | Nota donde el key scaling es neutro |
| `KLev` | KeyLevel | -1..1 | % con signo | Escalado del nivel por nota |
| `KTim` | KeyTime | -1..1 | % con signo | Escalado de los tiempos por nota |

### Amplitud: la envolvente al cuadrado **[P]**

La amplitud de la voz es `(envolvente · escalas de velocity y key · (Levl + Amp Gain))²`,
medido contra renders de NS2:
- Sustain 0.4 suena a −16 dB (0.4²), no a −8.
- Un ataque de 0.2 s está a −42 dB a los 10 ms y a −11 dB a los 100 ms.
- Con `VLev` 1, la velocity 40 suena a −20 dB del máximo.
- `Levl` 0.5 baja 6 dB, así que también está dentro del cuadrado.

Las otras envolventes son lineales: la de filtro ruteada al cutoff coincide con NS2 sin
elevarla al cuadrado.

Con Attack 0, la nota arranca a nivel completo desde la primera muestra **[P]**: una cuadrada
salta a su pico. El port calcula la amplitud por bloque y la interpola, así que en una voz
libre el primer bloque arranca ya en su valor final, en lugar de subir desde 0 en 64
muestras.

### Velocity y key scaling **[B]**

Se calculan una vez en el note-on (`0x1005314d4`, con la nota `n` y la velocity
`v ∈ 0..1`). `c` es `KCtr`:

```text
nivel_vel = VLev > 0 ? 1 + VLev·(v − 1) : 1 + VLev·v
nivel_key = n > c ? (KLev > 0 ? 1 − (n − c)·KLev/(128 − c) : 1)
                  : (KLev < 0 ? 1 + KLev·(1 − n/c)        : 1)
t_vel     = VTim·(64/127 − v)
t_key     = n > c ? (KTim > 0 ? (n − c)·KTim/c : 0)
                  : (KTim < 0 ? (n − c)·KTim/c : 0)
salida   ×= nivel_vel · nivel_key
tiempos  ×= 2^(3·(t_vel + t_key))
```

- Con `VLev = 0.4`, una nota con `v = 0.3` sale a 0.72 del nivel, no a 0.3.
- El factor de tiempos probablemente se aplica a todas las etapas **[I]**; la llamada es
  a `exp2f` **[I]**.

### Curvas (`AC`, `DC`, `SC`, `RC`)

El XML guarda directamente el **coeficiente `k`** de la curva, entre 1.03093 y 33.3333
**[P]**. Lo que muestra la UI (`cEden2ADSR::cCurveShape`) es otra escala **[B]**.

**Forma del segmento [B]**, decompilada de la función que dibuja la envolvente
(`0x1002ae5e0`):

```text
t ∈ [0, 1]              // fracción transcurrida del segmento
y(t) = y0 + (y1 - y0) · t / (t + (k - 1)·(1 - t))
```

| k | Forma |
|---|---|
| 2 | Recta |
| 1 < k < 2 | Convexa: sube rápido y se aplana (tipo carga RC). Default 1.22 |
| k → 1.03 | Casi un escalón al principio del segmento |
| k > 2 | Cóncava: arranca lento. A 33 casi todo el cambio queda al final |

El DSP de audio usa **la misma fórmula** (`0x10053163c`, el evaluador de envolventes
que también usan las conexiones FM) **[B]**.

**Tiempos en el DSP [B]**: internamente cada tiempo es un valor normalizado `x` en 0..1,
con `T = Tmax · x³` (curva cúbica). La modulación de Attack/Decay/Release/Delay se suma en
ese dominio `x` y se recorta a 0..1, y el segmento avanza `t += Δt / T` por muestra.

| Segmento | `Tmax` observado |
|---|---|
| Attack | 10 s |
| Delay, Decay | 20 s |

Si `T` es casi 0 se usa un mínimo de 1 ms. El XML guarda segundos, así que al cargar
`x = (T/Tmax)^(1/3)`. Esto explica los máximos de 10 s y 20 s de los presets.

**Conversión UI ↔ coeficiente [B]** (`0x1002aeff8` y su inversa `0x1002af08c`). Solo hace
falta para mostrar o editar el valor como lo hace NS2:

```text
// UI u ∈ [0,1] → k
b = 2u - 1
if |b| ≥ 1e-6:
    c = 4/3
    d = (b ≥ 0) ? (2u - 2)(1 - c) + b : 2u(c - 1) - b
    b = b / d
u' = clamp(0.5·b + 0.5, 0, 1)
k  = 1 / (0.03 + 0.94·u')
```

Valores de referencia: UI 0 → k 33.33, UI 0.25 → 6.78, UI 0.5 → 2 (recta),
UI 0.707 → 1.22051 (el default), UI 0.75 → 1.173, UI 1 → 1.031.

### Destinos de modulación de una envolvente **[P][B]**

`Attack`, `Decay`, `Sustain`, `Release`, `Level`, `Delay`, `Retrigger`; el Pitch Env
además tiene `Attack Level` y `Release Level`. `Retrigger` reinicia la envolvente
cuando la fuente cruza un umbral **[I]**. El port la reinicia cuando la entrada pasa de ≤ 0 a
\> 0 mientras la tecla está apretada; así Arping On (LFO 3 → Env 4 Retrigger) hace su ritmo
en semicorcheas como NS2. Algunos presets la disparan con LFOs Random de amplitud 0.01, así
que el umbral no puede ser mucho mayor que 0.

---

## LFOs (`cLFOSimple`)

5 por voz (`LFO1..LFO5`) **[M][P]**.

### Formas (`Type`)

| Tipo | Descripción **[M]** |
|---|---|
| `Sine` | Seno |
| `Square` | Cuadrada (`SqCv` la redondea **[I]**) |
| `Triangle` | Triángulo |
| `Saw Up` / `Saw Down` | Sierra |
| `Saw Double` | Doble sierra; la segunda se puede correr para hacer swing |
| `FM` | Onda FM de un operador, libre |
| `FM Sync` | Igual a FM, pero se reinicia en cada ciclo del LFO |
| `Random` | Sample & hold, con escalones o rampas |
| `Random Loop` | Random con una secuencia fija que se repite |

Todas se modifican con `Crv` (curve), `Warp` y `Qtz` (quantize) **[M]**.

### Parámetros

| Attr | Interno | Rango | Notas |
|---|---|---|---|
| `Rt` | Rate | 0..60 Hz | Curva UI `Power<300>` **[B]** |
| `BRt` | RateUnits | `Hz` o denominador de nota: `1` `2` `4` `8` `12` `16` `32` | Con un valor distinto de `Hz`, el período es `RtBt / BRt` de compás **[I]** |
| `RtBt` | RateBeats | 1..16 | Numerador en modo tempo |
| `Sync` | Sync | `OFF` `KEY` `GLOBAL` | Reinicio por nota o alineado a la canción **[M]** |
| `OutT` | OutputType | `BIPOLAR` `UNI HALF` `UNI FULL` | `y`, `max(y, 0)` (media onda) o `0.5·y + 0.5` **[B]** |
| `Atk` | Attack | 0..25 s | Fade-in **lineal** de la amplitud desde el note on: `amp *= t/Atk` mientras `t < Atk` **[B]** |
| `Ampl` | Ampl | 0..1 | Amplitud de salida |
| `PhOf` | PhaseOffset | 0..1 | Fase inicial |
| `Crv` | Curve | 0..1 | En el seno es un sesgo de fase (ver abajo) **[B]**; en las otras formas, el punto de quiebre **[I]** |
| `SqCv` | SqCurve | 0..1 | Suavizado de la cuadrada **[I]** |
| `Warp` | Warp | 0..1 | Curva racional sobre la salida, la misma de las envolventes con `k = 1/(0.03 + 0.94·Warp)`. 0.5 = neutro **[B]** |
| `Qtz` | Quantize | 0..9 (0 = OFF) | `y = floor(y·n + 0.5) / n`; no se aplica a Random Loop **[B]** |
| `RPtn` | RandPattern | 1..16 | Semilla o patrón del Random Loop |
| `RLen` | RandLoopLen | 2..16 | Largo en pasos del Random Loop |

### Detalles decompilados (`0x100560ccc`) **[B]**

- **Rate con modulación**: `rate = clamp(Rt + 60·m³, 0, 60)`, con `m` la modulación de
  Rate. El cubo da resolución fina cerca de 0.
- **Seno**: tabla de 512 puntos con interpolación lineal (índice = `|fase|·512/2π`).
- **Curve en el seno**: deforma la fase. Con `c` = Curve (+mod):
  `φ' = φ < c ? 0.5·φ/c : 0.5 + 0.5·(φ - c)/(1 - c)` y la salida es `sin(2π·φ')`. Con c = 0.5
  es el seno normal. Debajo de 0.02 y encima de 0.98 hay casos especiales (medio seno).
- **Tabla de saltos por tipo**, en el orden del enum: 0 Sine, 1 Square, 2 Triangle,
  3 Saw Up, 4 Saw Down (la misma rutina que Saw Up, negada), 5 Saw Double, 6 FM,
  7 FM Sync (comparte rutina con FM), 8 Random, 9 Random Loop.
- **Random**: LCG `x = (x · 31415821 + 1) & 0x3FFFFFFF`, salida `x·escala − 1` (bipolar).
  Random Loop usa una tabla de patrones de 16 pasos (`RPtn` elige el patrón, `RLen` el largo).
  Tempo: `RtBt 1`, `BRt 4` da un período de una negra **[P]**: 480 ms en el proyecto de NS2
  de la Mac, que está a **125 BPM**. El port toca a 120 BPM porque no tiene transporte.
  **Un período del LFO recorre el loop entero** **[P]**: cada paso dura `período / RLen`.
  Arping On (8/16 de compás, `RLen` 8) reinicia su envolvente en semicorcheas en NS2. Los
  valores del patrón siguen **[?]**: el port usa el LCG con una semilla por `RPtn`.
- **Warp**: `y' = y / (y + (k−1)(1−y))` para `y ≥ 0`, simétrica para `y < 0`, con
  `k = 1/clamp(0.03 + 0.94·Warp, 0.03, 0.97)`.
  El Warp solo se aplica a Sine, Triangle, Saw Up/Down y Saw Double; Square, FM y Random
  lo saltean.
- **Orden de proceso**: forma de onda → tipo de salida (`OutT`) → warp → quantize →
  amplitud (`Ampl` + mod, recortada a 0..1) → fade-in de `Atk`.

### ¿Por voz o global?

El manual dice "each voice has 5 LFOs" **[M]**. El trigger del LFO (`0x100560bf0`) **[B]**:

- `Sync = GLOBAL`: la fase sale de la posición del transporte.
- `Sync = KEY`: la fase arranca en `min(PhOf, 0.999)`.
- `Sync = OFF`: usa la fase que recibe como argumento. Si es negativa (nota nueva), toma
  una fase **aleatoria** con el mismo LCG de los LFOs, × 0.999. Cada nota arranca en un
  punto distinto.

### Destinos de modulación de un LFO **[P][B]**

`Rate`, `Level`, `Curve`, `Warp`, `Loop Pattern`.
