# Preguntas abiertas

Ordenadas por impacto en el sonido. Para cada una: qué falta y cómo averiguarlo.

Métodos:
- **D** = decompilar en el binario (Hopper vía REA). La UI formatea el valor
  normalizado para mostrarlo, así que la función `Format` de cada parámetro suele
  contener la fórmula. Ejemplo: `Eden2::cUIParamFormatCutoffFreq`.
- **R** = render de prueba en NS2: un patch mínimo, grabar la salida y medir.
- **X** = comparar presets o archivos de datos.

| # | Pregunta | Método | Dónde buscar |
|---|---|---|---|
| 1 | ~~`CutO` 0..1 → Hz~~ **Resuelto**: `20·900^v`, keytrack relativo a C3 ([04](04-filters.md#cutoff-fórmula-b)) | D | `0x1000a2c94`, `0x10055a290` |
| 2 | ~~`Q` 0..1 → resonancia~~ **Resuelto**: fórmulas por familia y ladder multimodo ([04](04-filters.md#q-y-resonancia-b)). El proceso por sample del ladder y el `Drive` (saturador cúbico) también están resueltos | D | `0x1005399d8`, `0x10054b098`, `0x100556498` |
| 3 | Topología: ladder `-A`, Formant (Peterson & Barney), WS (curvas Sine/Tri exactas, el resto aproximado) y Lo-Fi **resueltos**. Falta el Q de los formantes | D | `0x100549a90` |
| 4 | ~~Forma de los segmentos de envolvente~~ **Resuelto** (UI y DSP), y los tiempos `T = Tmax·x³` ([05](05-envelopes-lfos.md)) | D | `0x10053163c` |
| 5 | Escala de `Dept`. **Parcial**: `m = 1` = máximo del parámetro (filtro). La conversión `Dept → m` no aparece como `scvtf` del campo `+0x20` en el código estático; queda para el test de render | R | Test de render en [06](06-modulation.md#escalas) |
| 6 | ~~Cómo se suma la modulación~~ **Resuelto** para el filtro: en unidades del parámetro, con recorte al rango | D | `0x10055a290` |
| 7 | ~~Antialiasing de Analog~~ **Resuelto**: Sine/Tri/Saw/Square son `Analog.tbl` con mipmaps ([03](03-oscillators.md)). Falta el detalle de Pulse/Bi-Pulse | D | `0x100562188`, `0x1004e2234` |
| 8 | ~~`SRat` → ratio~~ **Resuelto**: `2^(4·SRat)`. Falta confirmar qué modo es `ANALOG` y qué hace la dureza | D | `0x100562188` |
| 9 | ~~NanoSaw~~ **Resuelto**: 7 sierras DPW, detunes y ganancias ([03](03-oscillators.md)) | D | `0x10046d1c4` |
| 10 | Fórmulas PD. **Parcial**: estructura, curvas de `PD` y límite por pitch. Faltan los quiebres exactos de Square/Pulse/Double Sine/Saw Pulse | D | `0x10056da2c` y rutinas |
| 11 | Selección de mip y crossfade en wavetable. Modos Detune/Spread **resueltos** ([03](03-oscillators.md)) | D | `0x10058cb60` |
| 12 | Campos `unknown_a`, `pairs` y `gain` del `.tbl` | D | Loader de `cWaveTable` |
| 25 | ~~FM~~ **Resuelto**: render, feedback y factor `Amt → entero` (`K·a²`) | D | `0x100565478` |
| 26 | Noise: `Cut`/`Q` **resueltos**. Falta confirmar si los tipos 3..6 filtran ruido blanco o browniano | D | `0x10056cd20` |
| 13 | ~~Unísono~~ **Resuelto**: tablas de detune, paneo y nivel por cantidad de voces ([02](02-preset-format.md#unísono-b)) | D | `cUnisonVoice`, `0x100559afc` |
| 14 | Glide: ¿exponencial en pitch? ¿tiempo constante o por intervalo? | D, R | `cSynthVoice` |
| 15 | `FMix` y `Ring` exactos | D | `cSynthOsc` |
| 16 | Ruteo `STEREO`: ¿cómo se reparten los osciladores mono? | D | `cFilters` |
| 17 | ~~Algoritmos de reverb~~ **Resuelto**: Freeverb (16 combs) + plate de Dattorro ([07](07-effects.md#topología-b)). Falta qué `Algo` usa cada motor | D | `0x1001907cc`, `0x1002b0ab8` |
| 18 | Multi-FX: mapeo de tipos **resuelto** ([07](07-effects.md)). Faltan rangos internos de delay/LFO por modo | D | `0x1002d1a20`, `0x100249b18` |
| 19 | ~~Capas `LL1..LX2`~~ **Resuelto** ([03](03-oscillators.md#capas-y-splits-por-velocity-osc--cosctriple-b)). Falta confirmar que `v = vel/127` y el `VEL` de la UI | D | `0x1005a6178` |
| 20 | Interpretación exacta de `FX/@Ord` | X, D | `cEden2Effects` |
| 21 | Bits de `Tags`: categorías inferidas por nombre ([02](02-preset-format.md)); falta confirmar en la UI y el resto de bits | X | Comparar presets con tags conocidos en la UI |
| 22 | ~~`Autm`~~ **Resuelto**: C02..C11 = Pad X, Pad Y, Knob 1..8 ([02](02-preset-format.md#autm)). Falta `C01` | X | — |
| 23 | `Poly=0` | R | Patch con Poly=0: contar voces |
| 24 | Unidades de `Dtn` (osc): **semitonos** (resuelto). `Tune` de zona: el render usa `Root − Tune`; falta confirmar que son semitonos | D, R | `0x10057020c` |

## Notas de método

- Hopper necesita unos 2'05" para analizar el binario. Con
  `REA_HOPPER_STARTUP_TIMEOUT_MS=600000` en el `env` del servidor MCP `rea`, REA lo
  espera sin problema.
- El binario usa chained fixups: los punteros de typeinfo y vtable no tienen xrefs en
  Hopper. Lo que funciona es entrar por **constantes** (escanear el binario buscando
  pares `movz`/`movk` con el float buscado) o por **strings de formato** (`"%0.01f Hz"`) y
  seguir sus xrefs desde el código.
- Las constantes calculadas al arrancar (como `K = log2(900)`) valen 0 en el archivo;
  hay que buscar el inicializador estático que las escribe (xrefs al global).
