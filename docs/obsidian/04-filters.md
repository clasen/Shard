# Filtros

- `F1`, `F2`: los dos filtros de cada voz.
- `F3`: el filtro global estéreo, aplicado a la suma de voces antes de los FX **[M][I]**.
  Que F3 sea el global se deduce de que su `KeyT` vale 1 en los 312 presets (no tiene
  nota a la que seguir) y de que el XML tiene 3 bloques para "2 por voz + 1 global".

Los tres usan la misma clase, `Audio::cSynthFilter` **[B]**: un filtro multimodo cuyo
`Type` elige también waveshapers, comb, formant y lo-fi.

## Parámetros

| Attr | Interno | Rango | Formato UI **[B]** | Notas |
|---|---|---|---|---|
| `En` | Enable | `On`/`Off` | | |
| `Type` | Type | ver lista | | |
| `CutO` | Cutoff | 0..1 | `cUIParamFormatCutoffFreq` | **`Hz = 20 · 900^CutO`** (20 Hz..18 kHz), ver abajo **[B]** |
| `Q` | Q | 0..1 | `cUIParamFormatQ` | Resonancia normalizada; el mapeo depende de la familia, ver abajo **[B]** |
| `Drv` | Drive | 0..1 | % | Saturador del ladder (`-A`) y ganancia de los waveshapers; los `-D` y EQ lo ignoran **[B]** |
| `KeyT` | KeyTrack | 0..2 | 0..200 % | `Hz *= (f_nota / 130.8128)^KeyT`: neutro en C3 (MIDI 48) **[B]** |
| `OGn` | OutputGain | 0..2 | dB, curva `Power<200>` | Ganancia lineal |
| `Pan` | OutputPan | 0..1 | pan | |
| `SGan` | ShelfGain | -24..24 | dB | Solo tipos EQ |
| `FVwl` | FormantVowel | 0..1 | % | Solo Formant: morph de vocal |
| `CMix` | CombMix | 0..1 | % | Mezcla para Comb y Lo-Fi ("Comb/Lo-Fi Mix") |
| `WSGn` | WSGain | 0..1 | % | Solo waveshapers; en los renders de NS2 no cambia nada **[P]** |

Los valores de los parámetros que no aplican al tipo elegido se guardan igual **[P]**.

## Cutoff: fórmula **[B]**

Decompilado de la función de formato de la UI (`0x1000a2c94` y sus gemelas para Noise y
Slate) y del update de coeficientes del filtro (`0x10055a290`):

```text
K      = log2(900)                      // inicializador estático: 1.442695 × ln(900) = 9.81378
hz     = 20 · 2^(K · (CutO + mod))      // = 20 · 900^(CutO + mod)
if note >= 0 and KeyT > 1e-6:
    hz *= (noteFreq[note] / 130.8128)^KeyT     // 130.8128 Hz = C3, MIDI 48
hz     = min(hz, 18000)
```

- `CutO = 0.5` da 600 Hz, `0.25` da 109.5 Hz y `0.75` da 3286 Hz.
- La modulación de cutoff se suma **en el dominio normalizado**, antes de la
  exponencial. Por eso una profundidad dada mueve siempre la misma cantidad de octavas:
  1.0 de modulación = log2(900) ≈ 9.8 octavas **[B]**.
- Los demás parámetros del filtro se suman a su modulación y se recortan a su rango
  (`fminnm`/`fmaxnm`) antes de usarse **[B]**.
- Los parámetros pasan por un slew interno para no producir saltos (constante `30 ·`
  sample-rate en el update) **[B]**. La constante exacta del slew **[?]**.

## Estructuras internas **[B]**

El patch del filtro (`cSynthFilter::cPatch`) guarda los floats en este orden, el mismo
del XML. Índices en palabras de 32 bits:

```text
[4] En  [5] Type  [6] CutO  [7] Q  [8] Drv  [9] SGan  [10] FVwl  [11] CMix
[12] WSGn  [13] KeyT  [14] OGn  [15] Pan
```

La modulación llega al filtro como un vector de 9 floats, en el orden de los `DstP`, y
cada uno se suma al parámetro y se recorta a su rango:

| # | DstP | Cómo se aplica |
|---|---|---|
| 0 | Cutoff | `CutO + m` (normalizado) |
| 1 | Cutoff Slow | `CutO + slew(m)`, con un limitador de pendiente proporcional a 30·Δt |
| 2 | Q | `Q + m` |
| 3 | Drive | `clamp(Drv + m, 0, 1)` |
| 4 | (Shelf Gain) | `clamp(SGan + 24·m, -24, 24)` dB |
| 5 | Formant Vowel | `clamp(FVwl + m, 0, 1)` |
| 6 | Comb/Lo-Fi Mix | `clamp(CMix + m, 0, 1)` |
| 7 | Output Gain | `clamp(OGn + 2·m, 0, 2)` |
| 8 | Output Pan | `clamp(Pan + m, 0, 1)` |

En todos los casos la modulación `m = 1` equivale al **valor máximo** del parámetro
(1, 2 o 24). Ver [06-modulation.md](06-modulation.md#escalas).

## Q y resonancia **[B]**

`q = clamp(Q + mod, 0, 1)`, suavizado. Después depende de la familia:

**Biquad (`-D`, en `0x1005399d8`)**:

```text
q' = q · min(1, f / 40 Hz)        // menos resonancia debajo de 40 Hz
Q_biquad = 0.5 + 5.5 · q'²        // LP/HP 12-D: de 0.5 a 6.0
```

Los tipos del diseñador son 1 = LP 2 polos, 2 = HP 2 polos, 3 = LP 1 polo, 4 = HP 1 polo,
8 = shelf y 12 = peak. Para EQ Peak, `Q = 0.25 + 7.75·q²` y la ganancia es `SGan` en dB.
El port usa el peaking de RBJ. **EQ Shelf [B][P]** (diseñador `0x10053aa74`, llamado desde
`0x10055a81c`):

```text
A    = 10^(SGan/40)
S    = clamp(Q², 0.01, 1)                    // pendiente: con Q 0 la transición ocupa décadas
low shelf RBJ con esa S, y los b multiplicados por 1/A
     → graves a +SGan/2 dB y agudos a −SGan/2 dB: un EQ de inclinación ("tilt")
```

El 1/A lo calcula el caller con una exponencial rápida por bits:
`bits = SGan·(−696658.5) + 1065292416 ≈ float(10^(−SGan/40))`.

Esto está medido contra NS2 con un saw:
- +12 dB con Q 0: de 0 dB a −6 dB.
- −12 dB con Q 0: de 0 dB a +6 dB.
- +24 dB con Q 1 y cutoff 0.5: de +12 dB a −12 dB, alrededor de 600 Hz.

Con este modelo, el port coincide en nivel y bandas. Antes se probó un high shelf con
pendiente 1, que subía 24 dB todo el espectro.

**Nivel del saturador del ladder [P]**: la voz alimenta los filtros a mitad de nivel. Para
que el Drive sature como en NS2, el ladder recibe el doble y su salida se divide por 2. Con
ese factor, los tests de Drive 0.3, 0.6 y 1.0 (oscilador a nivel 1 y 0.25) coinciden con NS2
en nivel y en bandas. Sin él, con Drive 1 el port salía hasta 5.6 dB más fuerte y más
brillante.

**F1 apagado (`En=Off`)** no filtra: en el port es `kFilterOff`, que solo aplica la ganancia.
Antes se aproximaba con un LP 24-A abierto, que perdía hasta 13 dB a 16 kHz.

**Modelo analógico (`-A`, setup en `0x10054b098`)**: un **ladder de 4 polos multimodo**.
La salida es una combinación lineal de la entrada y de las 4 etapas de 1 polo, como en el
Oberheim Xpander:

```text
fn   = clamp(f / sr, 0, 0.25)
d    = clamp(Drv + m, 0, 1)                  // Drive, suavizado en el estado del filtro (+0x10)
sat0 = 4.9·d³ + 0.1                          // ganancia antes del saturador
sat1 = (14.5·d³ + 1.5) / sat0                // ganancia después: el total es 14.5·d³ + 1.5
q' = q;  if f < 40 Hz: q' *= 0.5 + 0.0125·f
q' *= compensación por modo (abajo)
if q' ≥ 0.5: q' = 0.5 + 0.5·sqrt(2q' − 1)    // estira la mitad superior
w  = 2π·fn
c  = fn·(0.33049 + 0.13697·w² − 3.6557·fn)
G  = fn·(6.21533 + 0.86771·w²) + w·fn·(−2.72816 − 0.12692·w²)
k  = q' · (3.0087 + 3·c)                     // Notch: k = q'
```

`0x10055a290` le pasa el Drive suavizado al setup del ladder (`0x10054b098`) como cuarto
float **[B]**. Hay además un límite `k ≤ 3/(sat0·sat1)` que se salta si el byte `+0x1c`
del filtro vale 1 **[B]**. El constructor (`0x10055a1e8`) lo deja en 1, pero sin el límite el
ladder autooscila con Drive y Q moderados (Drive 0.55 y Q 0.5 ya da ×7 en todo el
espectro), así que Obsidian probablemente lo pone en 0. El port aplica el límite **[I]**.

Tabla de modos (`0x1009c3450`, 11 entradas de 32 bytes). Coeficientes de salida
`[x, y1, y2, y3, y4]`:

| Modo | Tipo | Ganancia | x | y1 | y2 | y3 | y4 | Compensación de q' |
|---|---|---|---|---|---|---|---|---|
| 1 | LP 12-A | 1 | 1 + 0.8k | 0 | 0.75 | 0 | 0 | `×(0.8 + 0.2d)`, y `÷ sat0` si > 1 |
| 3 | LP 24-A | 1 | 1 + 0.8k | 0 | 0 | 0 | 0.75 | `÷ sat0` si > 1 |
| 5 | BP 6-A | 0.75 | 0 | 2 | −2 | 0 | 0 | `×(0.82 − 0.22d)` |
| 6 | BP 12-A | 0.75 | 0 | 0 | 4 | −8 | 4 | `×(0.82 − 0.22d)` |
| 7 | HP 12-A | 0.7 | 1 | −2 | 1 | 0 | 0 | `×0.8`, y `÷ sat0` si > 1 |
| 8 | HP 24-A | 0.7 | 1 | −4 | 6 | −4 | 1 | `×(0.83 − 0.33d)`, y `÷ sat0` si > 1 |
| 9 | Notch 6-A | 0.7 | 1 | −2 | 2 | 0 | 0 | — |
| 10 | Notch 12-A | 0.7 | 1 | −4 | 8 | −8 | 4 | — |

- En los Notch no hay realimentación: `k = q'` escala la suma de las etapas (abajo).
- Con Drive alto (`sat0 > 1`, o sea `d > 0.57`) la resonancia de LP y HP baja en `1/sat0`.
- Los modos 2 y 4 son LP 12/24 sin el término `0.8k` y no los usa ningún tipo de Obsidian.
- Los coeficientes de HP, BP y Notch son exactamente los binomios `(1 − L)^n`, que es lo
  que uno espera de un ladder multimodo. El término `x·(1 + 0.8k)` de los LP es una
  compensación de graves: devuelve parte de la entrada para que la resonancia no
  "adelgace" el sonido **[I]**.
- **Proceso por sample [B]** (`0x10054b3f8` reparte; `0x100556498` renderiza los modos
  1..4, `0x1005565ac` los Notch y `0x1005566c8` el resto):

  ```text
  u  = (LP: (1 + 0.8k)·x;  resto: x) − k·y4          // Notch: u = x
  v  = clamp(sat0·u, −1, 1)
  s  = sat1·(v − v³/3)                                 // saturador cúbico
  yi = (1 − G)·yi + G·(0.769·in + 0.231·in_prev)       // 4 etapas; in = s, y1, y2, y3
  LP:    out = c2·y2 + c4·y4
  Notch: out = cx·s + k·(c1·y1 + c2·y2 + c3·y3 + c4·y4)
  resto: out = cx·s + c1·y1 + c2·y2 + c3·y3 + c4·y4
  ```

  `ci` son los coeficientes de la tabla de modos por la ganancia del modo. Con Drive 0 el
  saturador es casi lineal hasta `|u| ≈ 10` y la ganancia de pequeña señal es 1.5. Con Drive
  1, la ganancia es 16 (+24 dB) y la salida del saturador se recorta en `±2.13`. `G`, `k`
  y `cx` se **interpolan linealmente muestra a muestra** entre el bloque anterior y el actual.
- **Se procesan varias voces a la vez con SIMD (NEON)**: hay renders para 2 y para 4 voces
  en paralelo, y tres familias según el modo (≤4, ≤10 y el resto). En un ESP32 no hace
  falta: con una voz por vez alcanza.

## Tipos (`Type`)

Lista completa del binario, en el orden de la tabla de strings **[B]**, con el uso
total en F1+F2+F3 **[P]**:

| Familia | Tipos | Uso |
|---|---|---|
| Pasabajos | `LP 6-D`, `LP 12-D`, `LP 12-A`, `LP 24-A` | 15, 34, 465, 102 |
| Pasaaltos | `HP 6-D`, `HP 12-D`, `HP 12-A`, `HP 24-A` | 0, 2, 26, 24 |
| Pasabanda | `BP 6-A`, `BP 12-A` | 5, 14 |
| Notch | `Notch 6-A`, `Notch 12-A` | 1, 12 |
| EQ | `EQ Peak`, `EQ Shelf` | 66, 29 |
| Formant | `Formant-M`, `Formant-F` (voz masculina/femenina) | 14, 11 |
| Comb | `Comb Pos`, `Comb Neg` | 5, 13 |
| Waveshaper | `WS Tube`, `WS Soft`, `WS Hard`, `WS Enhance`, `WS Sine`, `WS Tri` | 19, 29, 23, 4, 9, 1 |
| Lo-Fi | `Lo-Fi` | 13 |

Código interno de cada tipo **[B]** (tabla `{u64 código, char* nombre}` en `0x100c2d4e0`,
con 25 entradas). El byte alto es la familia y el bajo la variante:

| Código | Tipo | Código | Tipo |
|---|---|---|---|
| `0x000` | LP 6-D | `0x400` | EQ Peak |
| `0x001` | LP 12-D | `0x401` | EQ Shelf |
| `0x002` | LP 12-A | `0x500` | Formant-M |
| `0x003` | LP 24-A | `0x501` | Formant-F |
| `0x100` | HP 6-D | `0x600` | Comb Pos |
| `0x101` | HP 12-D | `0x601` | Comb Neg |
| `0x102` | HP 12-A | `0x700` | WS Tube |
| `0x103` | HP 24-A | `0x701` | WS Soft |
| `0x200` | BP 6-A | `0x702` | WS Hard |
| `0x201` | BP 12-A | `0x703` | WS Enhance |
| `0x300` | Notch 6-A | `0x704` | WS Sine |
| `0x301` | Notch 12-A | `0x705` | WS Tri |
| | | `0x706` | Lo-Fi |

La función que calcula la respuesta del filtro (`0x10055b0d8`) reparte en dos
diseñadores de coeficientes distintos **[B]**:

- `0x10053bd3c` para `LP 6-D`, `LP 12-D`, `HP 6-D`, `HP 12-D`, `EQ Peak` y `EQ Shelf`. Es la
  familia "-D", la de un diseño digital clásico (biquad) **[I]**.
- `0x10054b5fc` para los `-A`: LP 12-A, HP 12-A, HP 24-A, BP 6/12-A y Notch 6/12-A. Es el
  modelo analógico. La rama de `LP 24-A` no se lee bien en el pseudocódigo **[?]**.
- Formant (`0x500/0x501`) y Comb (`0x600/0x601`) tienen funciones propias
  (`0x100549a90`, `0x1005419b8`).

Lo que dice el código sobre `-A`/`-D` confirma que son dos familias de implementación;
qué topología usa cada una **[?]**.

Nomenclatura:
- El número es la pendiente en dB/octava: 6 = 1 polo, 12 = 2, 24 = 4.
- `-A` y `-D` probablemente son "Analog" (modelado no lineal, tipo ladder/OTA con
  saturación) y "Digital" (biquad/SVF limpio) **[I]**. Todos los 6 dB de LP/HP son `-D`, y
  los 24 dB solo existen en `-A`.

## Formant, waveshapers y Lo-Fi **[B]**

**Formant-M / Formant-F** (respuesta en `0x100549a90`): 4 resonadores pasabanda en paralelo
cuyas frecuencias salen de una tabla de vocales (`0x1009c312c`), interpolada según
`FVwl`. La tabla es la de **Peterson & Barney (1952)**, con 10 vocales del inglés
americano. Columnas: F0, F1, F2, F3 en Hz.

| # | Hombres (Formant-M) | # | Mujeres (Formant-F) |
|---|---|---|---|
| 0 | 136, 266, 2293, 2937 (/i/) | 10 | 231, 310, 2783, 3312 (/i/) |
| 1 | 135, 392, 1993, 2568 (/ɪ/) | 11 | 232, 441, 2474, 3063 (/ɪ/) |
| 2 | 128, 526, 1854, 2481 (/ɛ/) | … | el resto de la tabla femenina sigue después |
| 3 | 125, 664, 1727, 2420 (/æ/) | | |
| 4 | 129, 631, 1192, 2377 (/ʌ/) | | |
| 5 | 124, 718, 1090, 2442 (/ɑ/) | | |
| 6 | 127, 568, 836, 2403 (/ɔ/) | | |
| 7 | 136, 437, 1023, 2245 (/ʊ/) | | |
| 8 | 139, 307, 876, 2239 (/u/) | | |
| 9 | 132, 489, 1360, 1709 (/ɝ/) | | |

Los valores son los publicados, así que un port puede tomarlos directamente del paper.

**Medido contra NS2 [P]** (ruido blanco por el filtro, tests `zm01`–`zm16`):
- Los cuatro resonadores están en **F0, F1, F2 y F3** de la vocal, interpolada linealmente
  entre las 10 de la tabla con `FVwl` (0..1 → vocal 0..9).
- **Las frecuencias escalan con el cutoff**: `f = f_tabla · Hz_cutoff / 800`. Con CutO 0.5
  (600 Hz) caen a 0.75 veces la tabla, con 0.7 a ~2.9 veces, y con CutO 1 se apilan en el
  techo de ~14.4 kHz.
- Cada resonador es un pasabanda de 2 polos con `Q_bp = 0.94·4^Q` y ganancia de pico
  `Q_bp^1.5` (F0 a 0.7 de eso). Con Q 0 la respuesta es una loma de +5 dB; con Q 0.5, picos
  de +13 a +15 dB.
- Como filtro global responde igual que en la voz.

Con este modelo (`syn::Formant`, 4 SVF en paralelo) los tests con CutO 0.5 quedan a ±0.4 dB
de nivel; con CutO 0.2–0.3 el port sale 2–3 dB más fuerte, y con CutO 1 más. Abierto
**[?]**: en Dirty Saws (Formant-M global con `OGn` 0.25 y LFO → Formant Vowel) NS2 atenúa
~7 dB y el port ~16 dB.

**WS Tube … WS Tri**: reutilizan el efecto **WaveShape** de NanoStudio (`0x1000fd398`)
con la forma 1..6. Cada forma es una **tabla de 1025 floats** generada al arrancar
(`0x1000fcc94`): se parte de una rampa lineal de −1 a 1, se aplica la curva de la forma
(`0x1000fd848`, que usa `expf`, `sinf` y `asinf` con constantes propias), y después se
normaliza por el pico y se le resta el valor central para quitar el DC. Curvas sobre la
rampa `x ∈ [−1, 1]` **[B]**, salvo donde dice **[I]**:

Las curvas, decompiladas de `0x1000fd848` (la tabla de saltos en `0x1006db3ea` da el
orden 1 Tube, 2 Soft, 3 Hard, 4 Enhance, 5 Sine, 6 Tri) **[B]**. `knee(a, e, r², c)` es
`c + sqrt(r² − (min(a, e) − e)²)`: un codo circular que une la recta con un techo plano.

| Forma | Curva sobre `x ∈ [−1, 1]` |
|---|---|
| WS Tube | `y = 0.5·(x + 1.41)² − 1`; si `y > 0.9`: `knee(y, 1.1413, 0.11655, 0.6587)`; si `y < −0.3`: `1.3891 − sqrt(5.7077 − (min(−y, 1.9891) − 1.9891)²)`. Asimétrica |
| WS Soft | `1 − 2/(e^(clamp(18.984·x, ±10)) + 1)` = `tanh(9.49·x)` |
| WS Hard | `v = 15·x`; si `|v| ≤ 0.8`: `v`; si no, `sign·knee(|v|, 1.2826, 0.46594, 0.31741)` |
| WS Enhance | si `|x| < 0.1`: `(10·|x|)²·x`. Después, para `y ≥ 0`: `v = 1.7065·y`, `knee(v, 1.7065, 2.912, −0.70648)` pasado 0.5; para `y < 0`: `v = 1.1413·y`, `−knee(|v|, 1.1413, 0.11655, 0.6587)` pasado 0.9 |
| WS Sine | `sin(13.5·x)` |
| WS Tri | `asin(sin(13.5·x))` |

Cada tabla tiene 1025 puntos sobre la rampa (paso 1/512), se divide por su pico y se le resta
el valor central (`0x1000fcc94`).

**Ganancias** (`0x1000fd398`) **[B]**: `d = a + (1 − a)·Drive`, con `a = 0.15` para Tube
y Enhance y `0.02` para el resto. La posición en la tabla es `512 + x·WSGn·1023·d`, y la
salida se multiplica por `1 − 0.3·d` (×1.5 en Tube). Que el primer factor sea `WSGn` y que
el índice esté centrado en 512 es **[I]**. El efecto `WaveShape` completo (`0x100438424`)
además sobremuestrea ×4 u ×8 y mezcla seco/húmedo; dentro del filtro de Obsidian eso
**[?]**.

**Modelo medido [P]** (27 tests contra NS2, en
[`tools/ref/gen.py`](../../firmware/tools/ref/gen.py), `zw*`, `zu*`, `zq*`):
- **`WSGn` no hace nada**: con 0.25, 0.5, 0.905 y 1, NS2 da renders idénticos.
- **El Cutoff es la mezcla seco/procesado**: `y = (1 − c)·x + c·WS(x)`, con `c` el cutoff
  normalizado más sus modulaciones (incluida la envolvente de filtro), sin keytrack. Con
  CutO 0 sale la señal sin tocar, con 0.5 exactamente la mitad de cada una.
- La ganancia de entrada es proporcional a `d`, ~4.5 veces la de la fórmula de arriba con
  `WSGn` 0.5. La salida sigue `1 − 0.3·d` (×1.5 en Tube). `OGn` escala la salida.
- **Como filtro global** (F3), la entrada es ~5 veces menor y la salida ~5 veces mayor: el
  WS global es casi lineal y su ganancia crece con `d` (Soft con Drive 0.3 sube 17 dB).
- Con esto los tests coinciden en nivel (±2.4 dB) y armónicos (±1.4 dB), salvo el folder
  Sine (4.7 dB de diferencia en el espectro).

**`Drive` en los demás tipos [B]**: el valor pasa por un limitador de pendiente y se guarda en el estado del filtro (`+0x10`, en `0x10055a290`). Lo usan el
ladder, como saturador (ver arriba), y el Formant (`0x706` se lo pasa a `0x1003d5884`). Los
biquad (`0x1005399d8`) y los EQ reciben 1 fijo. Las familias de render son 1 biquad
(`0x10053b208`), 2 ladder (`0x10054b3f8`), 3 formant, 4 comb (`0x100541890`) y 5
(`0x1003d58a4`), según `0x10055aca0`.

**Lo-Fi**: reutiliza el efecto **LoFi** de NanoStudio (`BitCrush`, `Decimate`, `Mix`);
`CMix` es la mezcla.

**Comb Pos / Comb Neg** **[B]**: setup en `0x10055aa90` (Pos) y `0x10055a874` (Neg), que
llaman a `0x100541828`. El proceso por muestra es `0x100433730`. `0x1005419b8` solo
dibuja la curva en la UI.

```text
f    = cutoff en Hz (misma fórmula y keytrack que los demás tipos, 20..18000)
D    = sr / f                                // muestras, en 16.16; máx. 2047 (buffer de 2048)
q    = clamp(Q + mod, 0, 1)
fb   = q / (q + 0.0618034·(1 − q))           // 0 si q < 1e-6; recortado a ±0.999
dry  = 1 − 0.3·CMix
wet  = dry·CMix
Neg: fb = −fb, wet = −wet

por muestra:
  tap = delay[D]                             // interpolación lineal
  h   = hp·h + (tap − tap_prev)              // bloqueador de DC, hp = 1 − 2π·5/sr
  buf = in + fb·h
  out = dry·in + wet·h
```

- La constante 1.0618034 (`c` en `0x100d11d08`) la fija un inicializador estático.
- `D`, `dry`, `wet` y `fb` se interpolan linealmente a lo largo del bloque. Para el
  retardo hace un crossfade entre la lectura con el `D` anterior y la nueva.
- Con `Q = 0` es un peine solo feedforward: `0.7·in ∓ 0.7·in[n−D]` con `CMix = 1`.

## Modulación propia

Destinos de la matriz para un filtro **[P][B]**: `Cutoff`, `Cutoff Slow`, `Q`, `Drive`,
`Output Gain`, `Output Pan`, `Formant Vowel`, `Comb/Lo-Fi Mix`.

`Cutoff Slow` es la misma modulación de cutoff pero suavizada (con un slew) **[I]**.
Sirve para fuentes con saltos, como los knobs o el random.

`Dest` puede ser `Filter 1`, `Filter 2`, `Filter 1+2` (los dos a la vez) o `Global Filter`.

## Cutoff muy bajo y bloqueo de DC **[P]**

Con el LP casi cerrado (CutO ≤ 0.1), el port coincide con NS2 en los armónicos de régimen.
Antes era más oscuro por la fase de la sierra ([03](03-oscillators.md)): el golpe del ataque
pasa por el filtro. Queda una diferencia en ese golpe. La cadena de NS2 tiene un bloqueo de
DC de **~5 Hz** (un pasa-altos de un polo): la sierra abierta de NS2 pasada offline por un
LP de 4 polos a 39 Hz reproduce exactamente su render con CutO 0.1, y la del port solo
coincide si se le agrega ese pasa-altos. No se sabe si está en la voz o en la salida, y el
port no lo tiene: solo se nota en el ataque de filtros casi cerrados.

## Ruteo

Ver [01-architecture.md](01-architecture.md#ruteo-de-filtros-rout-mp). Uso: STEREO 121,
PARALLEL 97, SERIES 94 **[P]**.

## Para un port

- Los modelos `LP 12-A` y `LP 24-A` solos cubren el 70 % de los usos de F1 (220/312).
- Un SVF de Cytomic/ZDF da LP/HP/BP/Notch de 12 dB con un solo núcleo. Dos en cascada
  dan los 24 dB. Para el sabor "-A" alcanza con un `tanh` (o una aproximación
  polinómica) en la entrada y en la realimentación.
- Los waveshapers son funciones sin memoria: baratos, pero con aliasing si no se hace
  oversampling.
