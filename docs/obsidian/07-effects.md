# Efectos

La cadena tiene 3 unidades después del filtro global, en un orden configurable (`FX/@Ord`,
ver [02-preset-format.md](02-preset-format.md#fx)) **[M][P]**. `FX/@En=0` saltea toda la
cadena. Cada unidad tiene además su propio `En`.

Son versiones "Simple" de los efectos de inserción de NanoStudio **[M][B]**.

## Multi-FX (`MFX` → `cEffectMultiFXSimple`)

| Attr | Interno | Valores |
|---|---|---|
| `En` | Enable | 0/1 |
| `Type` | Type | `Chorus 1` `Chorus 2` `Ensemble 1` `Ensemble 2` `Flanger 1` `Flanger 2` `Phaser 1` `Phaser 2` |
| `Rat` | Rate | 0..1 |
| `Dep` | Depth | 0..1 |
| `Dly` | Delay | 0..1 (sin uso en Phaser **[I]**) |
| `FB` | Feedback | 0..1 |
| `Wid` | Width | 0..1 |
| `Mix` | DryWetMix | 0..1 |

Uso: Chorus 1 162, Chorus 2 45, Phaser 2 32, Phaser 1 29, Ensemble 1 21, Ensemble 2 14,
Flanger 2 6, Flanger 1 3 **[P]**.

**Implementación [B]** (`0x10054da34`): dos motores, uno de línea de retardo modulada
(chorus/flanger, `0x1002d1a20`) y otro de phaser (`0x100249b18`):

| Type | Motor | Detalle |
|---|---|---|
| Chorus 1 | retardo modulado | modo 0 |
| Chorus 2 | retardo modulado | modo 1 |
| Ensemble 1 | 2 × retardo modulado | modo 1; el segundo con rate y delay ×0.707; salida repartida en un canal extra (mezcla 0.5) |
| Ensemble 2 | 2 × retardo modulado | modo 1; el segundo con rate y delay ×0.707; los dos sumados |
| Flanger 1 | retardo modulado | modo 2 |
| Flanger 2 | retardo modulado | modo 3 |
| Phaser 1 | phaser | flag 0 |
| Phaser 2 | phaser | flag 1 |

- Parámetros que recibe el motor de retardo: `Rat`, `Dep`, `Dly`, `FB` y `Mix` (+mod,
  recortado a 0..1). El phaser recibe `Rat`, `Dep`, `FB`, `Wid` y `Mix`.
- El motor interno trabaja en punto fijo (fase de 16 bits) y tiene una versión
  especializada por modo. Los rangos de delay y rate de cada modo **[?]**.

**Medido contra NS2 [P]** (tests `zf19`–`zf27`, `zf40`–`zf48`). En cada ventana de 20 ms se
ajusta la salida como `a·seco + b·seco(t − d)`:
- **Chorus 1** con `Mix` 1 suena seco 0.66 + húmedo 0.68, y con `Mix` 0.5 seco 0.82 + húmedo
  0.33, o sea `seco = 1 − Mix/3` y `húmedo = 2/3·Mix`. Con `Dly` 0.5 el retardo barre
  6.6–8.6 ms con un LFO de ~0.3 Hz (`Rat` 0.25); con `Dep` 1 el barrido es ~2.5 veces más
  rápido, y con `Rat` 0.75 el LFO es de ~1.5 Hz. La suma mono se explica con una sola copia
  retardada, así que L y R siguen el mismo LFO.
- **Flanger 1** mezcla igual que el chorus; su retardo está en 2.2 ms con `Dly` 0.1 y en
  5.2 ms con `Dly` 0.5, y `FB` realimenta el húmedo (una segunda copia en 2·d).
- **Phaser 1/2** con `Mix` 1 suena como seco 0.8 + húmedo 0.8.
- Chorus 2 y los Ensemble dan copias invertidas o varias a la vez: el ajuste no los
  resuelve **[?]**.

**En el port** (`syn::Chorus`): una línea modulada por canal con un LFO senoidal:
`centro = 3 + 10·Dly` ms (flanger: `1.45 + 7.5·Dly`), `profundidad = ±3.4·Dep` ms (flanger
±3), `rate = 0.12·e^(3.36·Rat)` Hz. Chorus 2 tiene el doble de rate y 1.5 veces la
profundidad, y los Ensemble suman una segunda línea con rate y retardo ×0.707 **[I]**.
`Wid` no se usa. Con eso los 18 tests quedan a ±1 dB de nivel.

La versión completa (`cEffectMultiFX`) tiene parámetros separados por tipo
(`ChorusRate`, `FlangerDelay`, `PhaserFeedback`…) y `BassBypass` **[B]**. Las variantes 1/2
de la versión simple probablemente son presets fijos de esos parámetros internos **[I]**.

## Delay (`Dly` → `cEffectDelaySimple`)

| Attr | Interno | Valores |
|---|---|---|
| `En` | Enable | 0/1 |
| `Type` | Type | `Stereo`, `Cross 1:1` … `Cross 1:4`, `Pingpong 1:1` … `Pingpong 1:4` |
| `DTim` | DelayTime | segundos (0.0003..1.31 observado) cuando `TDen=sec` |
| `TNum` | TimeBeats | 1..4, numerador en modo tempo |
| `TDen` | TimeUnits | `sec` o denominador de nota `1` `2` `4` `8` `12` `16` |
| `EQCt` | EQCut | 0..1, filtro en la realimentación |
| `FB` | Feedback | 0..0.85 observado |
| `Mix` | DryWetMix | 0..1 |

- `1:N` probablemente es la relación de tiempos entre los canales L y R **[I]**.
- `Cross` realimenta cada canal en el otro; `Pingpong` alterna los ecos entre L y R **[I]**.
- La UI usa una curva propia para el tiempo (`cEden2DelayUI::cUIParamCurveDelayTime`) **[B]**.

### Implementación **[B]**

El Delay simple comparte el proceso del Delay completo (`cEffectDelay`, `0x1005af4a4`):

- **Dos líneas, L y R.** La relación de tiempos sale de `RatioNum / RatioDen`: es lo que
  codifican los tipos `1:1` … `1:4`.
- **Tipo 2 (Pingpong)** usa otra configuración de ruteo que Stereo/Cross; los dos flags
  de ruteo se invierten según `Type == 2`.
- **Realimentación**:
  - `Stereo` (tipo 0): cada línea se realimenta a sí misma, con `FBBalance` (`b`)
    repartiendo `FB·min(1, 2b)` a una y `FB·min(1, 2 − 2b)` a la otra (ley lineal, centro
    en 0.5). No hay realimentación cruzada.
  - `Cross` y `Pingpong`: la auto-realimentación es 0 y todo el `FB` va **cruzado** a la
    otra línea.
- **Tiempo modulado**: la modulación de Delay Time se cuantiza en pasos de `1/64` y se
  limita a 1..64 subdivisiones.
- **EQ de la realimentación**: `LowCut` y `HighCut` (en la simple, `EQCut`) entran **al
  cuadrado** como coeficiente del filtro.

**Medido contra NS2 [P]** (tests `zf02`–`zf08`, `zf50`, con notas cortas):
- **El seco no baja**: sigue en 1 con cualquier `Mix`. El primer eco sale a `≈1.1·Mix²`
  (−12 dB con 0.5, 0 dB con 1).
- `Delay Mix` desde la matriz se suma a ese nivel húmedo, no a `Mix`: con `Mix` 0 y +0.5
  desde un knob, el eco sale a −6 dB.
- `FB` 0.5 baja 6 dB por eco, así que el feedback es lineal.
- **Cross y Pingpong 1:2** dan lo mismo en mono: la entrada va a la línea L (`DTim`) y cada
  eco rebota a la otra línea, de `DTim/2`. Con 250 ms, los ecos caen en 250, 375, 625,
  750 ms…
- **`EQCt`** es un shelving suave por pasada: con 0.1 baja ~3 dB los agudos por encima de
  ~400 Hz, y con 0.9 ~3 dB los graves por debajo de ~700 Hz.
- 1/8 a tempo da ecos cada 240 ms, porque el proyecto de NS2 de la Mac está a 125 BPM.

**En el port** (`syn::Delay`): ese modelo, con dos líneas de hasta 6 s en PSRAM después del
Multi-FX y antes de la reverb. Los tiempos a tempo se tocan a 120 BPM. Los tests quedan a
±1 dB por eco y por banda.
- En los presets el uso es Cross 1:2 (56), Stereo (45) y Pingpong 1:2 (22); la mediana de
  `EQCt` es 0.5.

## Reverb (`Rev` → `cEffectReverbAlgoSimple`)

| Attr | Interno | Valores |
|---|---|---|
| `En` | Enable | 0/1 |
| `Type` | Type | `Small Room 1` `Small Room 2` `Large Room 1` `Large Room 2` `Dense Room 1` `Dense Room 2` `Plate 1` `Plate 2` `Hall` |
| `Decy` | Time | 0..1 |
| `HDmp` | Damping | 0..1 |
| `LCut` | LoCut | 0..1 |
| `HCut` | HiCut | 0..1 |
| `Mix` | DryWetMix | 0..1 |

- Los 9 tipos coinciden con los presets de la Algoverb completa que trae `Install.zip`
  (`Effects/Algoverb/*.Algoverb`: Dense room 1/2, Hall, Large room 1/2, Plate 1/2,
  Small room 1/2) **[P]**. Esos presets son XML de una sola línea con los parámetros
  completos:

  ```xml
  <!-- Hall.Algoverb -->
  <Root FileVersion="1" V="1" Algo="Plate" PDel="0.35" PDMx="0.303334" HCut="0.2"
        LCut="0.453333" Decy="0.42" HDmp="0.188333" Dry="1" Wet="1" Widt="0.658333" />
  ```

  Hay **4 algoritmos base** (`Small Room`, `Large Room`, `Dense Room`, `Plate`), y los 9
  presets son variantes **[P]**:

  | Preset | Algo | PDel | PDMx | Widt |
  |---|---|---|---|---|
  | `* 1` (Small/Large/Dense Room 1, Plate 1) | el del nombre | 0 | 1 | 0 |
  | `* 2` (Small/Large/Dense Room 2) | el del nombre | 0.0421 | 0.495 | 0.638 |
  | `Plate 2` | Plate | 0.0928 | 0.402 | 0.638 |
  | `Hall` | **Plate** | 0.35 | 0.303 | 0.658 |

  En los 9, `HCut=0.2`, `LCut=0.453`, `HDmp=0.188`, `Dry=Wet=1` y `Decy≈0.5` (0.42 en Hall).
  La hipótesis es que el `Type` de la reverb simple elige algoritmo + `PDel`/`PDMx`/`Widt`
  de esta tabla, y que el resto de sus parámetros sale del preset de Obsidian **[I]**.
  Para un port alcanza con implementar los 4 algoritmos.
- La Algoverb completa tiene además `PDelay`, `PDelayMix` y `Width` **[B]**.
- `PDel` está en **segundos**: el buffer de pre-delay se reserva para `0.35·sr` muestras
  (redondeado a potencia de 2), o sea un máximo de 350 ms, que es justo el valor del Hall
  **[B]**.

### Topología **[B]**

Tanto la reverb simple como la completa usan el módulo interno **`Algoverb`**
(`0x1003edef8`). Tiene un pre-delay, filtros LoCut/HiCut (dos biquads), los parámetros
`Size`, `Damping`, `Width`, `Dry`, `Wet`, y **dos motores**:

**Motor A: banco de combs estilo Freeverb** (`0x1001907cc`):

- 16 combs con filtro pasabajos en la realimentación (LBCF), 8 por canal. Los largos
  base son los de Freeverb (Jezar), en muestras a 44.1 kHz:

  ```text
  1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
  ```

  escalados por `sr/44100`. El segundo grupo de 8 se desafina ±5 % alternando
  (impares +5 %, pares −5 %) para decorrelacionar los canales. Mínimo 16 muestras.
- Además reserva un buffer para los 4 tiempos de los difusores de Dattorro (ver abajo)
  más dos de 11.36 ms y 12.22 ms. Probablemente son allpasses de entrada **[I]**.
- Damping: corte del pasabajos de las combs en `22000 − 21200·Damping` Hz.

**Motor B: plate de Dattorro** (`0x1002b0ab8`). Es la plate del paper de J. Dattorro
("Effect Design, Part 1", JAES 1997), con los tiempos del original (pensados para
29761 Hz) expresados en segundos:

| Elemento | Tiempo | Muestras a 29761 Hz (Dattorro) |
|---|---|---|
| Difusores de entrada 1..4 | 4.762, 3.583, 12.721, 9.297 ms | 142, 107, 379, 277 |
| Tanque L: allpass modulado | 22.562 ms | 672 |
| Tanque L: delay 1 | 149.433 ms | 4453 |
| Tanque L: allpass 2 | 60.408 ms | 1800 |
| Tanque L: delay 2 | 124.830 ms | 3720 |
| Tanque R: allpass modulado | 30.454 ms | 908 |
| Tanque R: delay 1 | 141.519 ms | 4217 |
| Tanque R: allpass 2 | 89.138 ms | 2656 |
| Tanque R: delay 2 | 106.145 ms | 3163 |
| Excursión de modulación | 0.272 ms | ±8 |

- Los difusores de entrada se escalan por un factor y los del tanque por otro; el más
  probable es `Size` **[I]**.
- Hay **dos configuraciones** del motor B. Comparten la función de tamaños pero tienen
  constantes distintas: `{6.5625 ms, 0.6}` y `{2.734 ms, 0.75}`. Probablemente son
  tiempo y coeficiente de difusión **[I]**.
- Hay 4 LFOs de modulación internos (no solo los 2 del paper) **[B]**.

**Qué algoritmo usa qué motor [B]** (proceso `0x1003ee7e4`; enum `Algo`: 0 Small Room,
1 Large Room, 2 Dense Room, 3 Plate):

| Algo | Motor | Variante |
|---|---|---|
| Small Room | A (Freeverb) | modo 0 |
| Large Room | A (Freeverb) | modo 1 |
| Dense Room | B (Dattorro) | configuración `{2.734 ms, 0.75}` |
| Plate | B (Dattorro) | configuración `{6.5625 ms, 0.6}` |

Y "Hall" es Plate con pre-delay largo (ver la tabla de presets de arriba).

**Resto del proceso [B]:**

- Patch de la Algoverb en orden del XML: `[4] Algo [5] PDel [6] PDMx [7] HCut [8] LCut
  [9] Decy [10] HDmp [11] Dry [12] Wet [13] Widt`.
- **Pre-delay**: `PDel·sr` muestras. `PDMx` hace crossfade entre la señal directa y la
  pre-demorada antes de entrar a la reverb: `in + PDMx·(delayed − in)`.
- **Filtros**, con nombres contraintuitivos:
  - `HCut` controla un **pasaaltos** en `20 + 12000·HCut³` Hz (con 0.2 → 116 Hz).
  - `LCut` controla un **pasabajos** en `200 + 21000·(1 − LCut)³` Hz (con 0.453 → 3.6 kHz).

  Probablemente "HCut" quiere decir "corte de graves" y "LCut" "corte de agudos" al
  revés de la convención, o los nombres internos están cruzados con los de la UI **[I]**.
- `Decy` y `HDmp` llegan a los motores como tamaño/decay y damping.
- `Wet` y `Widt` entran al cuadrado cuando vienen modulados; la salida estéreo es una
  mezcla mid/side según `Widt`.

**Medido contra NS2 [P]** (tests `zf09`–`zf18`, `zf28`–`zf36`, `zf49`, con notas cortas):
- **Mezcla**: el seco se queda en 1 hasta `Mix` 0.5 y cae a 0 en `Mix` 1
  (`seco = min(1, 2·(1 − Mix))`); el húmedo crece lineal con `Mix`, igual si `Mix` viene de
  la matriz.
- **Caída** (dB/s con `Decy` 0.1 / 0.5 / 0.9): Plate, Plate 2 y Hall 81 / 27 / 5.9; Dense
  Room 168 / 57 / 17; Small Room >400 / 61 / 18; Large Room >400 / 81 / 35. Las Rooms con
  `Decy` 0.1 casi no suenan.
- `HDmp` 0.8 baja la cola entre 7 y 14 dB por encima de 1 kHz (Plate) y hasta 34 dB a
  8 kHz (Small Room).
- `LCut` y `HCut` 0.8 dejan el húmedo 26 dB más bajo: confirma un pasabajos y un pasaaltos
  con los cortes de arriba.
- El Hall es la Plate con pre-delay; Plate 2 y Room 2 apenas difieren de sus versiones 1.

**En el port**: Plate, Plate 2, Hall y Dense Room usan la plate de Dattorro, y Small y
Large Room usan Freeverb (8 combs y 4 allpasses por canal, con los 4 difusores de la plate
a la entrada; sin ellos, los primeros ecos salían 6 dB más fuertes que en NS2).
`obs2preset.py` traduce `Decy` a la realimentación de cada motor y fija la ganancia húmeda por
tipo (tablas `REVERB_DECAY` y `REVERB_GAIN` en tres puntos de `Decy`, ajustadas contra los
renders), `HDmp` al damping, `HCut`/`LCut` a filtros de un polo y el pre-delay según la tabla
de presets. Los tests quedan a ±2 dB de nivel húmedo y a ±3 dB/s de caída.

**Para un port**: Freeverb y la plate de Dattorro son algoritmos públicos y
documentados. Con los largos de arriba se reproducen las dos familias. En el ESP32
conviene una sola instancia global (no por voz) con las líneas en PSRAM: el tanque de la
plate son unos 0.36 s por lado (0.72 s en total), más los difusores y el pre-delay.

## Para un port

- La reverb es lo más caro. Con Freeverb (16 combs) y la plate de Dattorro se cubren los
  dos motores de NS2; en el ESP32 se puede empezar por la plate sola.
- El chorus y el delay son líneas de retardo con interpolación lineal; son baratos.
- Los efectos procesan la mezcla, no cada voz, así que su costo no crece con la
  polifonía.
