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

**En el port** (`syn::Delay`):
- Dos líneas de hasta 6 s en PSRAM, después del Multi-FX y antes de la reverb.
- `1:N` = la línea derecha N veces más larga que la izquierda **[I]**.
- Pingpong entra solo por la línea izquierda y se realimenta cruzado.
- `EQCt` es una inclinación: 0.5 no filtra, por debajo es un pasabajos de un polo y por
  encima un pasaaltos, con el coeficiente al cuadrado **[I]**.
- Seco `1 − 0.5·Mix` y húmedo `Mix`, como la reverb del port **[?]**.
- Los tiempos a tempo se tocan a 120 BPM. `Delay Mix` es destino de la matriz.
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
