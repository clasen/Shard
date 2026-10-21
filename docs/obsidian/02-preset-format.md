# Formato de preset `.obs`

## Contenedor

- Un preset es una **carpeta** `Nombre.obs/` que contiene `Package.obs` **[P]**.
- Si el preset usa samples, los `.wav` (o `.mp3`) viven en la misma carpeta **[P]**.
- `Package.obs` es **XML 1.0 en texto plano**, indentado con tabs **[P]**.
- La exportación de varios patches usa un `.zip` con estas carpetas adentro **[M][B]**.

## Convenciones generales

- Casi todo elemento lleva `V="n"`: versión del bloque. Un lector tiene que aceptar
  las versiones observadas (ver tabla) y tratar otras como desconocidas **[P]**.
- Los números se escriben en decimal con hasta 6 cifras significativas. Puede
  aparecer `INF` (por ejemplo, release infinito) **[P]**.
- Los booleanos aparecen como `On`/`Off`, `OFF`/`FILTER`, `0`/`1` o como enum en
  mayúsculas, según el bloque. Hay dos grafías para el mismo valor por versiones
  distintas: `Sync="OFF"` y `Sync="Off"` **[P]**.
- Los valores son de dos tipos **[P][B]**:
  - **unidades reales**: segundos, Hz, semitonos, ganancia lineal;
  - **normalizados 0..1**: cutoff, Q, drive, mezclas. La conversión a unidades reales
    vive en el motor ([09-open-questions.md](09-open-questions.md)).
- Los nombres de atributo son abreviaturas de los parámetros internos. La columna
  "Interno" sale del RTTI (`cAudioParam<Nombre>`) **[B]**.

## Árbol completo

```
Root (FileVersion=1, V=3, Tags, Oct)
├── Voice (V=2)                      → Eden2::cSynthVoice
│   ├── Osc (V=1, Type=Layer)        → Eden2::cOscTriple (capas por velocity)
│   │   ├── Osc0 / Osc1 / Osc2       → Audio::cSynthOsc
│   │   │   ├── Anlg                 → cOscTypeAnalogue
│   │   │   ├── Wave → WT            → cOscTypeWaveTable / cWaveTable
│   │   │   ├── PD                   → cOscTypePD
│   │   │   ├── FM → OP{O1,O2,O3}, Con{O11,O12,O13,O1O,O22,O23,O2O,O3O → Env}
│   │   │   ├── NS                   → cOscTypeNanoSaw
│   │   │   ├── Nois                 → cOscTypeNoise
│   │   │   └── Samp → Zones → Zone* → oscilador Sample
│   ├── Filt (V=1)                   → Eden2::cFilters
│   │   └── F1 / F2 / F3             → Audio::cSynthFilter (F3 = filtro global [I])
│   ├── Env (V=1)
│   │   └── EnvA / EnvF / EnvP / Env4 / Env5   → Audio::cEnvelopeADSR
│   ├── LFO (V=2)
│   │   └── LFO1 … LFO5              → Audio::cLFOSimple
│   └── Mod (V=2)
│       ├── C00 … C23                → conexiones de modulación (24 slots)
│       └── UI → C00 … C09 (Name)    → nombres de los 10 macros
├── FX (V=1, En, Ord)                → Eden2::cEden2Effects
│   ├── MFX                          → cEffectMultiFXSimple
│   ├── Dly                          → cEffectDelaySimple
│   └── Rev                          → cEffectReverbAlgoSimple
└── Autm (C01 … C11)                 → valores de los controles de performance [I]
```

Las tres ramas de oscilador guardan **todos** los sub-bloques de tipo (`Anlg`, `Wave`,
`PD`, `FM`, `NS`, `Nois`, `Samp`) aunque solo se use el que indica `Type` **[P]**.
Así, al cambiar el tipo en la UI se conservan los ajustes del otro.

## `Root`

| Attr | Valores | Significado |
|---|---|---|
| `FileVersion` | 1 | Versión del contenedor **[P]** |
| `V` | 3 | Versión del bloque **[P]** |
| `Tags` | entero 32 bits | Máscara de tags del browser **[P]**. Por correlación con los nombres de fábrica **[P][I]**: bit 0 lead (9 de 9), 1 pad, 2 bajo (39 de 41), 10 teclas, 19 batería (24 de 27), 8 efectos/FX; 20 y 29 están en casi todos; el resto describe timbre |
| `Oct` | 2..7 | Octava del teclado (`cEden2::Octave`) **[P][B]** |

## `Voice` → `cSynthVoice`

| Attr | Interno | Valores | Notas |
|---|---|---|---|
| `Poly` | Polyphony | 0..16 | Límite de voces. **0 es mono** (legato con `GLeg`/`GTim`): en NS2, un acorde sobre Deep Bass (`Poly=0`) suena como una sola nota **[P]**. El port trata 0 y 1 como mono, con unísono |
| `Uni` | Unison | 1..4 | Voces por nota **[P]** |
| `UDtn` | UnisonDetune | 0..1 | Ver [Unísono](#unísono-b) **[B]** |
| `USte` | UnisonStereo | 0..1 | Apertura estéreo del unísono, ver [Unísono](#unísono-b) **[B]** |
| `GLeg` | GlideLegato | `On`/`Off` | **[P]** |
| `GTim` | GlideTime | segundos, 0..0.47 observado | Curva UI `Power<400>` **[P][B]** |

### Unísono **[B]**

`Eden2::cUnisonVoice` (note-on en `0x100559afc`, tablas en `0x100c2d230 + 24·N`). Para
la voz `k` de `N`:

```text
d      = clamp(UDtn + mod, 0, 1)
pitch += det[N][k] · d² · (1 + 0.2·N)            // semitonos
pan    = clamp(pan_base + pan[N][k] · clamp(USte + mod, 0, 1), 0, 1)
nivel *= scale[N]
```

| N | `scale` | `det` | `pan` |
|---|---|---|---|
| 1 | 1.0 | 0 | 0 |
| 2 | 0.85 | −1, 1 | −0.5, 0.5 |
| 3 | 0.75 | −1, 0, 1 | −0.5, 0, 0.5 |
| 4 | 0.7 | −1, −0.5, 0.5, 1 | −0.5, 0.25, −0.25, 0.5 |

`scale` cuenta **al cuadrado**, como la amplitud que escala **[P]**: con 2 y 4 voces
desafinadas (`UDtn` 0.3), NS2 suena al mismo nivel que con una (0.85² · √2 ≈ 1, 0.7² · √4 ≈ 1).
Con `scale` lineal, el port salía 1.6 dB más fuerte con 2 voces.

**Ley de paneo [P]**: en NS2, `USte` no cambia el nivel de la suma mono (2 y 4 voces con
`USte` 1 y 0.5 suenan como con 0), y un oscilador paneado a cualquier lado tampoco. O sea
que la ley es de suma constante, `L = 2·(1 − pan)` y `R = 2·pan`, y no de balance. El port
la usa para el unísono. Para el `Pan` de los filtros sigue con la ley de balance (unidad en
el centro), porque In Lazy Curls, con F1 y F2 paneados a los extremos, solo coincide así.

Con `UDtn = 1` y 2 voces el detune es ±1.4 semitonos; con valores típicos (0.1–0.3) es de
pocos cents. La ley de paneo **[?]**.

## `Osc` → `cOscTriple` (capas y splits por velocity)

`Type="Layer"` en los 312 presets de fábrica; los presets de usuario usan también
`Split 2` y `Split 3` (enum `VelSplitType`) **[P][B]**. Fórmulas en
[03](03-oscillators.md#capas-y-splits-por-velocity-osc--cosctriple-b).

| Attr | Interno | Valor por defecto observado |
|---|---|---|
| `LL1` / `LL2` | VelLayerLevel1/2 | 0 |
| `S2L1` | VelSplit2Level1 | 0.5 |
| `S3L1` / `S3L2` | VelSplit3Level1/2 | 0.333 / 0.667 |
| `LX1` / `LX2` | VelXFade1/2 | 0 |

## `Osc0..2` → `cSynthOsc` (parámetros comunes)

| Attr | Interno | Rango observado | Notas |
|---|---|---|---|
| `En` | Enable | `On`/`Off` | |
| `Type` | Type | `Analog` `Wavetable` `PD` `FM` `NanoSaw` `Noise` `Sample` | |
| `Trn` | Transpose | -36..36 | Semitonos enteros |
| `Dtn` | FineTune | ±0.2 | Formato UI "cents", curva `SigmoidCentreZero<20>` **[B]**. Probablemente en semitonos (±0.2 = ±20 cents) **[I]** |
| `KeyT` | KeyTrack | 0..1 | 1 = sigue el teclado; formato UI 0..200 % **[B]** |
| `Lev` | Level | 0..2 | Ganancia lineal (UI en dB, curva `Power<200>`) **[P][B]** |
| `Pan` | Pan | 0..1 | 0.5 = centro |
| `FMix` | FilterMix | 0..1 | Reparto de la salida entre F1 y F2 **[I]** |
| `Ring` | RingMod | 0..1 | Ring mod con el oscilador anterior **[I]** |
| `Widt` | Width | 0..1 | Ancho estéreo |
| `POff` | PhaseOffset | 0..1 | Fase inicial (UI en grados 0..360) **[B]** |

Los sub-bloques por tipo están en [03-oscillators.md](03-oscillators.md).

## `Filt` → `cFilters` y `F1..F3` → `cSynthFilter`

| Attr | Interno | Valores |
|---|---|---|
| `Rout` | FilterRouting | `SERIES` `PARALLEL` `STEREO` |
| `O1BP` | Osc1Bypass | `OFF` `FILTER` (el manual también menciona saltear la envolvente de amplitud) |

Para `F1..F3` ver [04-filters.md](04-filters.md).

## `Env/EnvA..Env5` → `cEnvelopeADSR`

`EnvA` = amplitud, `EnvF` = filtro, `EnvP` = pitch, `Env4`, `Env5` libres **[P][B]**.
Detalle en [05-envelopes-lfos.md](05-envelopes-lfos.md).

## `LFO/LFO1..5` → `cLFOSimple`

Detalle en [05-envelopes-lfos.md](05-envelopes-lfos.md).

## `Mod/C00..C23`

24 slots de conexión **[P]**:

```xml
<C00 V="3" Src="Filter Env" Dest="Filter 1+2" DstP="Cutoff" Sca="None" Dept="100" />
```

| Attr | Significado |
|---|---|
| `Src` | Fuente |
| `Dest` | Módulo destino |
| `DstP` | Parámetro del destino (`---` si `Dest="None"`) |
| `Sca` | Multiplicador: un macro que escala la conexión, o `None` |
| `Dept` | Profundidad, -100..100 |

Un slot vacío es `Src="None" Dest="None" DstP="---"`. Listas completas en
[06-modulation.md](06-modulation.md).

`Mod/UI/C00..C09` guarda solo `Name`: los nombres de los 10 macros (8 knobs + Pad X +
Pad Y) **[P][M]**.

## `FX`

| Attr | Interno | Valores |
|---|---|---|
| `En` | Enable | 0/1, bypass de toda la cadena |
| `Ord` | EffectOrder | Entero con 3 bytes empaquetados |

`Ord` en hexadecimal **[P]**: `0x020100` (301 presets), `0x010200`, `0x020001`,
`0x000102`, `0x000201`. Siempre es una permutación de {0, 1, 2}, un byte por
efecto. Que byte *n* sea la posición del efecto *n* o el efecto en la posición *n*
**[?]**. Tomando los hijos en el orden del XML (MFX=0, Dly=1, Rev=2), el valor por
defecto `0x020100` da MFX → Dly → Rev en cualquiera de las dos lecturas.

Detalle de cada efecto en [07-effects.md](07-effects.md).

## `Autm`

```xml
<Autm C01="0.5" C02="0" … C11="0" />
```

Son 11 valores 0..1. `C01` vale 0.5 en los 312 presets **[P]**; puede ser el pitch wheel
(centro 0.5) **[I]**. `C02..C11` son las posiciones de los 10 controles de performance en el
mismo orden que sus nombres en el preset (`C00..C09 Name`): **Pad X, Pad Y, Knob 1..8**
**[P]**. Al cargar el preset, los controles toman esos valores.

Evidencia: los nombres de Rave Lead ("Cutoff", "Q", "Attack", "Release", "Bottom Cut"…)
coinciden con sus slots Pad X → Cutoff, Pad Y → Q, Knob 1 → Attack. Con este orden, los
renders del port de Rave Lead, Ef Em Crystal Keys, Deep Bass, BassGroove y Africa Brass
pasan a coincidir con NS2. También se explican los tests: `Autm` C02 = 1 mueve el Pad X y no
el Knob 1, y C10 = 1 mueve el Knob 7 y no un pad.

## Versiones de bloque observadas

| Bloque | V |
|---|---|
| Root | 3 |
| Voice, LFO, Mod, Env/EnvX | 2 |
| Mod/Cnn | 3 |
| Osc, OscN, Anlg, Wave, WT, PD, FM, OP/On, Con/Onn, NS, Nois, Samp, Zones, Filt, Fn, Env, LFOn, FX, MFX, Dly, Rev | 1 |

Un lector robusto: valida `FileVersion=1`, usa defaults para atributos ausentes y
avisa (sin fallar) cuando encuentra una versión de bloque no vista.

## Ejemplo mínimo (fragmento real)

```xml
<?xml version="1.0"?>
<Root FileVersion="1" V="3" Tags="538968202" Oct="4">
	<Voice V="2" Poly="8" Uni="2" UDtn="0.244583" USte="0.753667" GLeg="On" GTim="0.1">
		<Osc V="1" Type="Layer" LL1="0" LL2="0" S2L1="0.5" S3L1="0.333" S3L2="0.667" LX1="0" LX2="0">
			<Osc0 V="1" En="On" Type="Wavetable" Trn="0" Dtn="0" KeyT="1" Lev="0.772996" Pan="0.5" FMix="0" Ring="0" Widt="0.5" POff="0">
				<Anlg V="1" Wave="Pulse" Sync="On" PWM="0.31515" SRat="0.267917" SShp="0.399167" />
				<Wave V="1" Mode="Detune-2" Pos="0.510835" Dtn="0.245833">
					<WT V="1" Name="Complex/Shimmer" />
				</Wave>
				…
```
