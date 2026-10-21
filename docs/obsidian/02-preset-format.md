# The `.obs` preset format

## Container

- A preset is a **folder** `Name.obs/` that contains `Package.obs` **[P]**.
- If the preset uses samples, the `.wav` (or `.mp3`) files live in the same folder **[P]**.
- `Package.obs` is **plain-text XML 1.0**, indented with tabs **[P]**.
- Exporting several patches uses a `.zip` with these folders inside **[M][B]**.

## General conventions

- Almost every element carries `V="n"`: the block version. A reader has to accept the
  observed versions (see the table) and treat others as unknown **[P]**.
- Numbers are written in decimal with up to 6 significant digits. `INF` can appear (for
  example, an infinite release) **[P]**.
- Booleans appear as `On`/`Off`, `OFF`/`FILTER`, `0`/`1` or as an upper-case enum, depending
  on the block. There are two spellings of the same value across versions: `Sync="OFF"` and
  `Sync="Off"` **[P]**.
- Values are of two kinds **[P][B]**:
  - **real units**: seconds, Hz, semitones, linear gain;
  - **normalized 0..1**: cutoff, Q, drive, mixes. The conversion to real units lives in the
    engine ([09-open-questions.md](09-open-questions.md)).
- Attribute names are abbreviations of the internal parameters. The "Internal" column comes
  from the RTTI (`cAudioParam<Name>`) **[B]**.

## Full tree

```
Root (FileVersion=1, V=3, Tags, Oct)
├── Voice (V=2)                      → Eden2::cSynthVoice
│   ├── Osc (V=1, Type=Layer)        → Eden2::cOscTriple (velocity layers)
│   │   ├── Osc0 / Osc1 / Osc2       → Audio::cSynthOsc
│   │   │   ├── Anlg                 → cOscTypeAnalogue
│   │   │   ├── Wave → WT            → cOscTypeWaveTable / cWaveTable
│   │   │   ├── PD                   → cOscTypePD
│   │   │   ├── FM → OP{O1,O2,O3}, Con{O11,O12,O13,O1O,O22,O23,O2O,O3O → Env}
│   │   │   ├── NS                   → cOscTypeNanoSaw
│   │   │   ├── Nois                 → cOscTypeNoise
│   │   │   └── Samp → Zones → Zone* → Sample oscillator
│   ├── Filt (V=1)                   → Eden2::cFilters
│   │   └── F1 / F2 / F3             → Audio::cSynthFilter (F3 = global filter [I])
│   ├── Env (V=1)
│   │   └── EnvA / EnvF / EnvP / Env4 / Env5   → Audio::cEnvelopeADSR
│   ├── LFO (V=2)
│   │   └── LFO1 … LFO5              → Audio::cLFOSimple
│   └── Mod (V=2)
│       ├── C00 … C23                → modulation connections (24 slots)
│       └── UI → C00 … C09 (Name)    → names of the 10 macros
├── FX (V=1, En, Ord)                → Eden2::cEden2Effects
│   ├── MFX                          → cEffectMultiFXSimple
│   ├── Dly                          → cEffectDelaySimple
│   └── Rev                          → cEffectReverbAlgoSimple
└── Autm (C01 … C11)                 → values of the performance controls [I]
```

The three oscillator branches store **all** the per-type sub-blocks (`Anlg`, `Wave`, `PD`,
`FM`, `NS`, `Nois`, `Samp`) even though only the one named by `Type` is used **[P]**. That way,
changing the type in the UI keeps the settings of the other.

## `Root`

| Attr | Values | Meaning |
|---|---|---|
| `FileVersion` | 1 | Container version **[P]** |
| `V` | 3 | Block version **[P]** |
| `Tags` | 32-bit integer | Browser tag mask **[P]**. By correlation with the factory names **[P][I]**: bit 0 lead (9 of 9), 1 pad, 2 bass (39 of 41), 10 keys, 19 drums (24 of 27), 8 effects/FX; 20 and 29 are set in almost all; the rest describe timbre |
| `Oct` | 2..7 | Keyboard octave (`cEden2::Octave`) **[P][B]** |

## `Voice` → `cSynthVoice`

| Attr | Internal | Values | Notes |
|---|---|---|---|
| `Poly` | Polyphony | 0..16 | Voice limit. **0 is mono** (legato with `GLeg`/`GTim`): in NS2, a chord on Deep Bass (`Poly=0`) sounds as a single note **[P]**. The port treats 0 and 1 as mono, with unison |
| `Uni` | Unison | 1..4 | Voices per note **[P]** |
| `UDtn` | UnisonDetune | 0..1 | See [Unison](#unison-b) **[B]** |
| `USte` | UnisonStereo | 0..1 | Stereo spread of the unison, see [Unison](#unison-b) **[B]** |
| `GLeg` | GlideLegato | `On`/`Off` | **[P]** |
| `GTim` | GlideTime | seconds, 0..0.47 observed | UI curve `Power<400>` **[P][B]** |

### Unison **[B]**

`Eden2::cUnisonVoice` (note-on at `0x100559afc`, tables at `0x100c2d230 + 24·N`). For voice `k`
of `N`:

```text
d      = clamp(UDtn + mod, 0, 1)
pitch += det[N][k] · d² · (1 + 0.2·N)            // semitones
pan    = clamp(pan_base + pan[N][k] · clamp(USte + mod, 0, 1), 0, 1)
level *= scale[N]
```

| N | `scale` | `det` | `pan` |
|---|---|---|---|
| 1 | 1.0 | 0 | 0 |
| 2 | 0.85 | −1, 1 | −0.5, 0.5 |
| 3 | 0.75 | −1, 0, 1 | −0.5, 0, 0.5 |
| 4 | 0.7 | −1, −0.5, 0.5, 1 | −0.5, 0.25, −0.25, 0.5 |

`scale` counts **squared**, like the amplitude it scales **[P]**: with 2 and 4 detuned voices
(`UDtn` 0.3), NS2 plays at the same level as with one (0.85² · √2 ≈ 1, 0.7² · √4 ≈ 1). With a
linear `scale`, the port came out 1.6 dB louder with 2 voices.

**Pan law [P]**: in NS2, `USte` does not change the level of the mono sum (2 and 4 voices with
`USte` 1 and 0.5 sound as with 0), and neither does an oscillator panned to either side. So the
law is constant-sum, `L = 2·(1 − pan)` and `R = 2·pan`, not a balance law. The port uses it for
the unison. For the filters' `Pan` it keeps the balance law (unity at the centre), because In
Lazy Curls, with F1 and F2 panned hard apart, only matches that way.

With `UDtn = 1` and 2 voices the detune is ±1.4 semitones; with typical values (0.1–0.3) it is a
few cents.

## `Osc` → `cOscTriple` (velocity layers and splits)

`Type="Layer"` in all 312 factory presets; user presets also use `Split 2` and `Split 3` (enum
`VelSplitType`) **[P][B]**. Formulas in
[03](03-oscillators.md#velocity-layers-and-splits-osc--cosctriple-b).

| Attr | Internal | Default value observed |
|---|---|---|
| `LL1` / `LL2` | VelLayerLevel1/2 | 0 |
| `S2L1` | VelSplit2Level1 | 0.5 |
| `S3L1` / `S3L2` | VelSplit3Level1/2 | 0.333 / 0.667 |
| `LX1` / `LX2` | VelXFade1/2 | 0 |

## `Osc0..2` → `cSynthOsc` (common parameters)

| Attr | Internal | Observed range | Notes |
|---|---|---|---|
| `En` | Enable | `On`/`Off` | |
| `Type` | Type | `Analog` `Wavetable` `PD` `FM` `NanoSaw` `Noise` `Sample` | |
| `Trn` | Transpose | -36..36 | Whole semitones |
| `Dtn` | FineTune | ±0.2 | UI format "cents", curve `SigmoidCentreZero<20>` **[B]**. Probably in semitones (±0.2 = ±20 cents) **[I]** |
| `KeyT` | KeyTrack | 0..1 | 1 = follows the keyboard; UI format 0..200 % **[B]** |
| `Lev` | Level | 0..2 | Linear gain (UI in dB, curve `Power<200>`) **[P][B]** |
| `Pan` | Pan | 0..1 | 0.5 = centre |
| `FMix` | FilterMix | 0..1 | Split of the output between F1 and F2 **[I]** |
| `Ring` | RingMod | 0..1 | Ring mod with the previous oscillator **[I]** |
| `Widt` | Width | 0..1 | Stereo width |
| `POff` | PhaseOffset | 0..1 | Start phase (UI in degrees 0..360) **[B]** |

The per-type sub-blocks are in [03-oscillators.md](03-oscillators.md).

## `Filt` → `cFilters` and `F1..F3` → `cSynthFilter`

| Attr | Internal | Values |
|---|---|---|
| `Rout` | FilterRouting | `SERIES` `PARALLEL` `STEREO` |
| `O1BP` | Osc1Bypass | `OFF` `FILTER` (the manual also mentions bypassing the amplitude envelope) |

For `F1..F3` see [04-filters.md](04-filters.md).

## `Env/EnvA..Env5` → `cEnvelopeADSR`

`EnvA` = amplitude, `EnvF` = filter, `EnvP` = pitch, `Env4` and `Env5` free **[P][B]**. Details in
[05-envelopes-lfos.md](05-envelopes-lfos.md).

## `LFO/LFO1..5` → `cLFOSimple`

Details in [05-envelopes-lfos.md](05-envelopes-lfos.md).

## `Mod/C00..C23`

24 connection slots **[P]**:

```xml
<C00 V="3" Src="Filter Env" Dest="Filter 1+2" DstP="Cutoff" Sca="None" Dept="100" />
```

| Attr | Meaning |
|---|---|
| `Src` | Source |
| `Dest` | Destination module |
| `DstP` | Destination parameter (`---` if `Dest="None"`) |
| `Sca` | Multiplier: a macro that scales the connection, or `None` |
| `Dept` | Depth, -100..100 |

An empty slot is `Src="None" Dest="None" DstP="---"`. Full lists in
[06-modulation.md](06-modulation.md).

`Mod/UI/C00..C09` only stores `Name`: the names of the 10 macros (8 knobs + Pad X + Pad Y)
**[P][M]**.

## `FX`

| Attr | Internal | Values |
|---|---|---|
| `En` | Enable | 0/1, bypasses the whole chain |
| `Ord` | EffectOrder | Integer with 3 packed bytes |

`Ord` in hexadecimal **[P]**: `0x020100` (301 presets), `0x010200`, `0x020001`, `0x000102`,
`0x000201`. It is always a permutation of {0, 1, 2}, one byte per effect. Whether byte *n* is the
position of effect *n* or the effect at position *n* is **[?]**. Taking the children in XML order
(MFX=0, Dly=1, Rev=2), the default `0x020100` gives MFX → Dly → Rev under either reading.

Details of each effect in [07-effects.md](07-effects.md).

## `Autm`

```xml
<Autm C01="0.5" C02="0" … C11="0" />
```

These are 11 values in 0..1. `C01` is 0.5 in all 312 presets **[P]**; it may be the pitch wheel
(centre 0.5) **[I]**. `C02..C11` are the positions of the 10 performance controls in the same
order as their names in the preset (`C00..C09 Name`): **Pad X, Pad Y, Knob 1..8** **[P]**. When
the preset loads, the controls take these values.

Evidence: Rave Lead's control names ("Cutoff", "Q", "Attack", "Release", "Bottom Cut"…) match its
slots Pad X → Cutoff, Pad Y → Q, Knob 1 → Attack. With this order, the port's renders of Rave
Lead, Ef Em Crystal Keys, Deep Bass, BassGroove and Africa Brass match NS2. It also explains the
tests: `Autm` C02 = 1 moves Pad X, not Knob 1, and C10 = 1 moves Knob 7, not a pad.

## Observed block versions

| Block | V |
|---|---|
| Root | 3 |
| Voice, LFO, Mod, Env/EnvX | 2 |
| Mod/Cnn | 3 |
| Osc, OscN, Anlg, Wave, WT, PD, FM, OP/On, Con/Onn, NS, Nois, Samp, Zones, Filt, Fn, Env, LFOn, FX, MFX, Dly, Rev | 1 |

A robust reader validates `FileVersion=1`, uses defaults for missing attributes, and warns
(without failing) when it finds a block version it has not seen.

## Minimal example (real excerpt)

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
