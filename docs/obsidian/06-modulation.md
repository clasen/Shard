# Matriz de modulación

## Modelo

Cada patch tiene **24 slots** (`Mod/C00..C23`) **[P]**. Un slot es:

```
salida = fuente × (multiplicador ? valor_macro : 1) × Dept/100   →   suma al parámetro DstP de Dest
```

La forma multiplicativa del multiplicador sale del manual ("scales the connection's
value") **[M]**. Cómo se suma al parámetro destino (en el dominio normalizado 0..1 o en
unidades reales, con o sin recorte) **[?]**.

- `Dept` va de -100 a 100 **[P]**.
- Varios slots pueden apuntar al mismo destino; se suman **[I]**.
- Los slots vacíos (`None`) se pueden saltear. En los presets de fábrica el promedio
  de slots usados es de unos 17 de 24 **[P]**, pero muchos son conexiones de macros
  que no se mueven durante la nota.

## Fuentes (`Src`)

Lista completa del binario **[B]**, con el uso en los presets **[P]**:

| Fuente | Rango **[I]** | Por voz | Uso |
|---|---|---|---|
| `None` | — | — | 2223 |
| `Const One` | 1 | — | 0 |
| `Rand 1`, `Rand 2` | valor aleatorio fijo por nota | sí | 57, 37 |
| `Note` | nota MIDI normalizada | sí | 3 |
| `Velocity` | 0..1 | sí | 29 |
| `Release Vel` | 0..1 | sí | 0 |
| `Gate` | 0/1 | sí | 0 |
| `Amp Env`, `Filter Env`, `Pitch Env`, `Env 4`, `Env 5` | 0..1 (Bipolar ±1) | sí | 7, 342, 320, 78, 80 |
| `LFO 1` … `LFO 5` | según `OutT` | sí | 341, 355, 332, 192, 252 |
| `Pitch Wheel` | -1..1 | no | 304 |
| `Pitch Wheel +` / `Pitch Wheel -` | mitad positiva/negativa | no | 0 |
| `Knob 1` … `Knob 8` | 0..1 | no | 402, 397, 353, 306, 206, 154, 26, 23 |
| `Pad X`, `Pad Y` | 0..1 | no | 330, 339 |

El binario no tiene un string `Mod Wheel` ni `Aftertouch` cerca de esta tabla **[B]**.
Lo más probable es que el mod wheel MIDI se mapee a un knob por la tabla de MIDI CC
(`MIDI CONTROLLER MAP`) **[I]**.

## Multiplicador (`Sca`)

`None` o una fuente de macro (`Knob 1..8`, `Pad X`, `Pad Y`) **[P]**. Se usa en el 0.3 %
de las conexiones; es un recurso fino para "más vibrato cuanto más subo el knob".

## Destinos (`Dest`) y parámetros (`DstP`)

| Dest | DstP posibles **[P][B]** |
|---|---|
| `Osc All` / `Osc 1` / `Osc 2` / `Osc 3` | `Pitch Coarse`, `Pitch Fine`, `Level`, `Pan`, `Phase`, `Filter Mix`, `Ring Mod`, `Width`, `An Pulse Width`, `An Sync Ratio`, `An Sync Hardness`, `WT Position`, `WT Detune`, `PD Phase Dist`, `FM OP1->OP2`, `FM OP1->OP3`, `FM OP2->OP3`, `FM OP1 Freq`, `FM OP1 FB`, `FM OP1 Out`, `FM OP2 Freq`, `FM OP2 FB`, `FM OP2 Out`, `FM OP3 Freq`, `FM OP3 Out`, `NanoSaw Detune`, `NanoSaw Mix`, `Noise Decimate`, `Noise Cutoff`, `Noise Q`, `Sample Start`, `Sample Loop Start`, `Sample Loop Len`, `Sample Zone Shift` |
| `Filter 1` / `Filter 2` / `Filter 1+2` / `Global Filter` | `Cutoff`, `Cutoff Slow`, `Q`, `Drive`, `Output Gain`, `Output Pan`, `Formant Vowel`, `Comb/Lo-Fi Mix` |
| `Amp` | `Gain` |
| `Amp Env` / `Filter Env` / `Env 4` / `Env 5` | `Attack`, `Decay`, `Sustain`, `Release`, `Level`, `Delay`, `Retrigger` |
| `Pitch Env` | lo anterior + `Attack Level`, `Release Level` |
| `LFO 1..5` | `Rate`, `Level`, `Curve`, `Warp`, `Loop Pattern` |
| `Voice` | `Unison Detune`, `Unison Stereo`, `Glide Time` |
| `FX` | `Multi-FX Mix`, `Multi-FX Rate`, `Multi-FX Depth`, `Multi-FX Delay`, `Multi-FX FB`, `Delay Mix`, `Delay Time`, `Delay FB`, `Delay EQ`, `Reverb Mix`, `Reverb Time` |

`Osc All` en la práctica se usa casi solo para `Pitch Coarse`: 818 de 829 usos **[P]**.

## Destinos más usados (para priorizar en un port) **[P]**

| Conexión | Usos |
|---|---|
| `Filter 1+2 / Cutoff` | 556 |
| `Osc All / Pitch Coarse` | 818 |
| `Amp Env / Attack` y `Release` | 250 + 242 |
| `Amp / Gain` | 232 |
| `Filter 1+2 / Q` | 170 |
| `Filter 1+2 / Cutoff Slow` | 161 |
| `Global Filter / Cutoff` | 137 |
| `FX / Delay Mix`, `Reverb Mix` | 109 + 108 |

## Escalas

**Lo que está decompilado [B]:**

- `Dept` se lee del XML como **entero** (lector de int, rango int32) en el campo `+0x20`
  de la conexión (`0x1003ff868`). Los otros campos son `+0x10` Src, `+0x14` Dest, `+0x18`
  DstP y `+0x1c` Sca.
- Cada destino recibe la modulación ya sumada, como un vector de floats en el orden de
  sus `DstP`, y la **suma al valor del parámetro en sus propias unidades**. En el filtro:
  `m = 1` mueve el cutoff normalizado 1.0 (≈ 9.8 octavas), Q 1.0, Output Gain 2.0 y Shelf
  Gain 24 dB. Es decir, **`m = 1` equivale al valor máximo del parámetro** (detalle en
  [04-filters.md](04-filters.md#estructuras-internas-b)).
- En el LFO, la modulación de Rate entra al cubo: `Rt + 60·m³` (ver
  [05-envelopes-lfos.md](05-envelopes-lfos.md)).

**Lo que falta [?]:**

- Cómo se convierte `Dept` en `m` dentro de la matriz. La hipótesis natural es
  `m = Σ fuente × Dept/100 (× multiplicador)` **[I]**: con eso, `Filter Env → Cutoff`
  con `Dept=100` barre todo el rango, que es lo que se escucha en los presets.
- La escala de **Pitch Coarse**. La regla "m = 1 → máximo del parámetro" daría ±36
  semitonos (el máximo de `Trn`), pero los presets apuntan a `Dept` = semitonos (`m = 1`
  = 100 semitonos) **[P][I]**: 304 presets de fábrica doblan con `Pitch Wheel → Osc All
  Pitch Coarse, Dept 2` (el ±2 típico), y las Pitch Env de los hats suben una octava con
  `Dept 12`. El port usa 100. No se encontró la constante en el código del oscilador.

**Test de render para cerrarlo (5 minutos en NS2):**

1. Patch init: un oscilador Analog Sine, sin filtro, sin FX, nota A3.
2. Conexión `Pitch Env → Osc All / Pitch Coarse`, `Dept = 100`. EnvP en Bipolar con
   `SLv = 1`, attack largo (por ejemplo 2 s) y sustain 0.
3. Grabar la salida y medir la frecuencia al principio: `semitonos = 12·log2(f0/220)`.
4. Repetir con `Dept = 50` para confirmar que es lineal.

Patrón típico de fábrica que ayuda a calibrar **[P]**: los slots 0..3 suelen ser
`Filter Env → Filter 1+2/Cutoff`, `Pitch Env → Osc All/Pitch Coarse`, y los knobs 1..4 a
Cutoff, Q, Attack y Release (coincide con los nombres por defecto de los macros).
