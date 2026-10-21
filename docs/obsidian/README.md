# Obsidian (NanoStudio 2): especificación por ingeniería inversa

Documentación del sintetizador Obsidian de NanoStudio 2, pensada como base para
reimplementarlo en otra plataforma (web con AudioWorklet/WASM, ESP32-S3, desktop).
Describe *qué* hace el motor y *cómo* están guardados sus datos; no contiene código
ni assets de la app.

## Índice

| Archivo | Contenido |
|---|---|
| [01-architecture.md](01-architecture.md) | Flujo de señal, voz, módulos y mapa de clases internas |
| [02-preset-format.md](02-preset-format.md) | Formato `.obs` (XML) completo, atributo por atributo |
| [03-oscillators.md](03-oscillators.md) | Los 7 tipos de oscilador y sus parámetros |
| [04-filters.md](04-filters.md) | Filtros de voz, ruteo y filtro global |
| [05-envelopes-lfos.md](05-envelopes-lfos.md) | 5 envolventes y 5 LFOs |
| [06-modulation.md](06-modulation.md) | Matriz de modulación: fuentes, destinos, parámetros |
| [07-effects.md](07-effects.md) | Multi-FX, Delay, Reverb y su orden |
| [08-wavetable-format.md](08-wavetable-format.md) | Formato binario `.tbl` |
| [09-open-questions.md](09-open-questions.md) | Lo que falta saber y cómo averiguarlo |
| [10-porting-notes.md](10-porting-notes.md) | Notas para web y ESP32 |

## Fuentes

- **Presets de fábrica**: 312 archivos `Factory/Instruments/Obsidian/*.obs/Package.obs`
  dentro de `NanoStudio 2.app/Wrapper/NanoStudio 2.app/`. El esquema y los rangos se
  extrajeron recorriendo todos.
- **Manual**: `Manual/Obsidian.html` (v2.1.2) dentro del bundle.
- **Binario**: `NanoStudio 2` (Mach-O arm64, SHA-256 `310ed846…5a13`). Está stripped,
  pero conserva los nombres RTTI de C++ y las tablas de strings de la UI, de donde
  salen las clases, los nombres de parámetros, los formatos y las curvas de la UI.
- **Wavetables**: `WaveTables/**/*.tbl`, analizadas byte a byte.

## Niveles de confianza

Cada afirmación lleva una marca:

- **[P]** observado en los presets (dato concreto, verificable).
- **[M]** dicho por el manual.
- **[B]** observado en el binario (RTTI, strings), sin ejecutar ni decompilar.
- **[I]** inferencia razonable, no verificada.
- **[?]** desconocido; ver [09-open-questions.md](09-open-questions.md).

## Estado

- Hecho: arquitectura, formato de preset completo, enums completos, mapa
  atributo XML → parámetro interno, formato `.tbl` (estructura y muestras).
- Decompilado: fórmula del cutoff y del keytrack, códigos internos de los tipos de
  filtro, forma de las curvas de envolvente y conversión de la UI, detalles del LFO
  (rate, curve, warp, random, tipos de salida, quantize).
- También decompilado: Q y el ladder multimodo de los filtros `-A`, cómo suma la
  modulación en el filtro, el pitch de los osciladores, Analog (wavetable con mipmaps),
  NanoSaw (7 sierras DPW), PD (estilo CZ) y los modos de detune de la wavetable.
- También: FM (punto fijo, ondas de banda limitada), Noise (decimación que sigue la
  nota), reverb (Freeverb + plate de Dattorro) y Delay.
- También: proceso por sample del ladder, envolventes del DSP (`T = Tmax·x³`), Formant
  (Peterson & Barney), waveshapers, Multi-FX (tipos → motores) y qué algoritmo de reverb
  usa cada sala.
- Medido en hardware: costo por bloque en un ESP32-S3 real; ver
  [10-porting-notes.md](10-porting-notes.md).
- Pendiente: escala de `Dept` en la matriz (hay un test de render en
  [06](06-modulation.md#escalas)), curvas exactas de los waveshapers y rangos internos
  del Multi-FX. Ver
  [09-open-questions.md](09-open-questions.md).

## Nota legal

Leer el formato de los presets y reimplementar la arquitectura para uso propio es
interoperabilidad. Los presets de fábrica, las wavetables y los samples son
contenido de Blip Interactive: no redistribuirlos dentro de otro producto.
