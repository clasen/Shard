# Oscillators

There are 3 per voice (`Osc0..Osc2`; the UI and the matrix call them Osc 1..3). The common
parameters are in [02-preset-format.md](02-preset-format.md#osc02--csynthosc-common-parameters).
Each type is described below.

Use in the factory presets (Osc0 / Osc1 / Osc2) **[P]**:

| Type | Osc0 | Osc1 | Osc2 |
|---|---|---|---|
| Analog | 113 | 150 | 198 |
| Wavetable | 59 | 49 | 34 |
| FM | 55 | 37 | 13 |
| NanoSaw | 28 | 34 | 39 |
| PD | 23 | 11 | 7 |
| Noise | 21 | 27 | 18 |
| Sample | 13 | 4 | 3 |

If something has to be cut for small hardware, Analog + Wavetable + NanoSaw cover most of them.

---

## Common part: pitch and dispatch by type **[B]**

Decompiled from `cSynthOsc` (`0x1005812e0`). The oscillator patch stores the floats in XML
order: `[4] En [5] Type [6] Trn [7] Dtn [8] KeyT [9] Lev [10] Pan [11] FMix [12] Ring [13] Widt
[14] POff`.

```text
pitch = 60 + (note − 60)·KeyT + Trn + Dtn + mod_pitch    // semitones
pitch = clamp(pitch, 0, 127)
f     = 440 · 2^((pitch − 69)/12)
level = clamp(Lev + 2·mod_level, 0, 2) · (velocity gain)
```

- `KeyT` pivots on **C4 (MIDI 60)**: with `KeyT = 0` every note sounds as C4.
- `Dtn` is in **semitones**: ±0.15 = ±15 cents.
- Pitch modulation arrives in semitones; the matrix applies the `Dept → semitones` scale
  ([06-modulation.md](06-modulation.md#scales)).
- The oscillator's modulation vector: `[0]` pitch, `[1]` level, `[2]`/`[4]` pan and filter mix,
  `[3]` phase, `[5]` ring, `[6]` width, and then each type's own destinations.
- Glide: the pitch approaches the target with a maximum slope proportional to `100·Δt`, in
  semitones **[B]**. How it depends on `GTim` is **[?]**.

Dispatch by `Type` (a switch in the same order as the manual):

| Type | Function | Patch |
|---|---|---|
| 0 Analog | `0x100562188` | `+0x40` |
| 1 Wavetable | `0x10058cb60` | `+0x48` |
| 2 PD | `0x10056da2c` | `+0x50` |
| 3 FM | `0x100565478` | `+0x58` |
| 4 NanoSaw | `0x10046d1c4` | `+0x60` |
| 5 Noise | `0x10056c920` | `+0x68` |
| 6 Sample | `0x10056fddc` | `+0x70` |

## Analog (`Anlg` → `cOscTypeAnalogue`)

"Analog modelling oscillator with 6 waveforms, PWM and sync" **[M]**.

| Attr | Internal | Values |
|---|---|---|
| `Wave` | Waveform | Internal index: 0 `Sine`, 1 `Tri`, 2 `Saw`, 3 `Square`, 4 `Pulse`, 5 `Bi-Pulse` **[B][I]** |
| `Sync` | SyncMode | `OFF`, `ANALOG`, `DIGITAL`; older versions use `Off`/`On` |
| `PWM` | PulseWidth | 0.01..0.5 |
| `SRat` | SyncRatio | 0..1 normalized; UI format `cUIParamFormatSyncRatio` |
| `SShp` | SyncSharpness | 0..1; in the matrix it shows up as "An Sync Hardness" |

**How it works [B]** (`0x100562188`):

- **Sine, Tri, Saw and Square are not separate generators**: they are positions in a
  **band-limited wavetable**, `WaveTables/Analog.tbl`, with `position = wave/3`. The 4 frames of
  that file are exactly sine, triangle, saw and square (verified by their harmonic content:
  fundamental only; odd at 1/n²; all at 1/n; odd at 1/n). They play through the **same
  wavetable engine** as the Wavetable oscillator (`0x1004e2234`).
- The 4 frames of `Analog.tbl` have the same RMS, that of a sine with peak 1 (0.7071): the saw
  and the triangle peak at 1.22 and the square at 0.71 **[B]**. NS2 renders confirm it: saw,
  square, sine and tri play at the same level **[P]**.
- **Start phase [P]**: with `POff` 0, the saw starts at 0 and **rises** (jumping to −peak at
  half phase), the sine and the triangle start at 0 rising, and the square starts on +. The
  Pulse starts on its narrow positive segment. Measured on the first cycle of each note in NS2
  renders. With the saw inverted, a very closed LP gives a different attack thump, and the sum
  with other oscillators and FM changes.
- So **the antialiasing is the table's mipmaps** (44 levels, 4 per octave, see
  [08-wavetable-format.md](08-wavetable-format.md)). There is no BLEP.
- **Bi-Pulse, measured [P]**: a three-level wave, with a positive pulse of width `PWM` at the
  start of the cycle, a negative one half a cycle later and zero in between, so it has no even
  harmonics. The port builds it as the Pulse minus itself shifted by half a cycle, at half the
  Pulse's gain (NS2's level and spectrum at PWM 0.23).
- **Pulse (4)** and **Bi-Pulse (5)** use `pw = clamp(PWM + mod, …)` on the same engine, with
  their own constants (Pulse: 0.58 and a gain of 2/3; Bi-Pulse: 0.5 and 1). Most likely they
  come out as the difference of two saw reads `pw` apart **[I]**.
- **Sync** (internal to the oscillator; there is no sync between oscillators):
  `ratio = 2^(4 · clamp(SRat + mod, 0, 1))`, that is from 1× to 16×. One of the two active modes
  uses `SShp` (+mod) to compute, with `exp2`, an integer "hardness", probably the width of the
  smoothing window; in the other the hardness is fixed. That the first is `ANALOG` and the
  second `DIGITAL` is the natural reading **[I]**.
- `On` in older presets: mapping it to `ANALOG` is the reasonable choice **[?]**.

**For a port**: a wavetable oscillator with mipmaps covers Analog and Wavetable at once. The
band-limited sine, triangle, saw and square tables can be generated by additive synthesis
without NS2's file.

## Wavetable (`Wave` → `cOscTypeWaveTable`)

| Attr | Internal | Values |
|---|---|---|
| `Mode` | Mode | `Single` `Crossfade` `Detune-2` `Detune-4` `Spread-4` |
| `Pos` | Position | 0..1, position in the table (morph) |
| `Dtn` | Detune | 0..1, for the multi-voice modes |
| `WT/@Name` | — | `Category/Name`, for example `Simple/Sin-Tri-Saw`. Maps to `WaveTables/<Category>/<Name>.tbl` |

- `Single`: no interpolation between frames (jumps to the nearest frame) **[I]**.
- `Crossfade`: linear interpolation between neighbouring frames **[I]**. The most used mode
  (288/312).
- `Detune-2` **[B]**: 2 voices at `f·(0.99994 − 0.0619·d²)` and `f·(1.00006 + 0.0619·d²)`, with
  `d = Dtn` (+mod) clamped to 0..1. At `d = 1` that is ±6.2 %, close to ±1 semitone.
- `Detune-4` **[B]**: 4 voices in two pairs, `f·(1 ± 0.018·d')`, with `d'` a scaled version of
  `Dtn`.
- `Spread-4` **[B]**: 4 voices at `pitch + d·k_i` semitones (four different offsets), and the
  table position is adjusted.

Implementation decompiled at `0x10058cb60` **[B]**: the oscillator receives the pitch in
semitones relative to A4 (`f = 440·2^(p/12)`) and a mode 0..4 in enum order.

Available tables (25) **[P]**:
- `Analog.tbl`
- `Simple/`: Sin-Tri-Saw, Sin-Saw-Sqr, Sin-Sqr-Tri, Tri Vintage, Saw Vintage, Sqr Vintage, Saw
  Octaves, Resonant, Highpassed
- `Complex/`: Shimmer, Digitalis, Buzz, Chimes, Mineral, Hum, Tech, Zing
- `Instrument/`: Vocal, Organ, Brassy, Orchestral, EP, Bass, Bowed

Binary format in [08-wavetable-format.md](08-wavetable-format.md).

## PD, phase distortion (`PD` → `cOscTypePD`)

"Gives low-pass and resonant-filter sounds without needing a filter" **[M]**. It is Casio CZ
style. Decompiled at `0x10056da2c` and its routines **[B]**.

| Attr | Internal | Values |
|---|---|---|
| `Wave` | Waveform | 0 `Saw`, 1 `Square`, 2 `Pulse`, 3 `Double Sine`, 4 `Saw Pulse`, 5 `Res Saw`, 6 `Res Fat Saw`, 7 `Res Tri`, 8 `Res Tri HP` |
| `PD` | PD | 0..1, amount of phase distortion (+ mod "PD Phase Dist") |

**How it works:**

- The phase is a **32-bit** integer accumulator (`inc = f/sr · 2³²`) and the output is read
  from a 2048-point cosine table (constants `2047.5/d`).
- Each waveform computes, once per block, the **break points** of the phase transfer function,
  in 16-bit fixed point (`65535·d`), and the slopes of each segment. For the saw the bend is the
  classic CZ one: `φ' = φ < d ? 0.5·φ/d : 0.5 + 0.5·(φ − d)/(1 − d)` and then `cos(2π·φ')`.
- **`PD` goes through a per-waveform curve** before it becomes a break point. For Saw:
  `a = PD < 0.25 ? 0.08·PD : 1.3067·PD + c`, then `x = a^(1/4)` and `d = 0.5 − 0.5·x` (from 0.5,
  which gives a sine, to almost 0, which gives the saw). The others use another exponent, each
  with its own initial linear segment: Square `√`, Pulse `^0.3`, Double Sine `^0.4` and Saw
  Pulse `^0.15`.
- **Pitch-dependent antialiasing**: between 65 Hz and 8372 Hz (C9) a polynomial in the
  frequency **limits the maximum amount of distortion**, so high notes have fewer harmonics.
  There are also two render variants, chosen by frequency (threshold at
  `16384·f/sr·… > 16`).
- **Resonant shapes (5..8)**: a sine at `r` times the frequency, with `r = 1 + k·PD²`, windowed
  by a saw or triangle envelope that restarts every cycle (the CZ's "resonant filter"). `r` is
  limited so the resonance stays below Nyquist: above 130 Hz the limit is `(−5·f + c)/f`, and
  below it is 64. The window phase is corrected per shape: 0.3/r (Res Saw), 0.25/r (Res Fat
  Saw), `(ceil(r/2) − 0.25)/r` or 0.67/r (Res Tri and Res Tri HP).
- In the resonant shapes, when the phase resets at note-on, the first samples ramp up from 0 to
  avoid clicks (a fade of `0.0004·sr` samples, about 0.4 ms).
- The constant `c` of the Saw curve comes from asking for continuity at `PD = 0.25`:
  `0.08·0.25 = 1.3067·0.25 + c` gives `c = −0.3067`, and then `a = 1` at `PD = 1` **[I]**.
- In the presets, waveform use is Saw 895, Square 10, Saw Pulse 7, Res Fat Saw 6, Pulse 6, Res
  Tri HP 5, Res Saw 3, Double Sine 3 and Res Tri 1 **[P]**.

## FM (`FM` → `cOscTypeFM`)

It has 3 operators. All three can be carriers; **only 1 and 2 can be modulators**, including of
themselves (feedback) **[M]**.

### Operators `FM/OP/O1..O3` (`cOperator`)

| Attr | Internal | Values |
|---|---|---|
| `Wave` | Waveform | `Sine` `Saw Up` `Saw Down` `Square` `Triangle` `Parabola` `Half Wave` `Full Wave` `Sine 1+2` `Sine 1+3` `Sine 1+4` `Sine 1+5` `Rez 2` `Rez 3` `Rez 5` |
| `Rat` | Ratio | Integer. **ratio = Rat · 0.0001** **[B]** (10000 = 1:1). With modulation it is clamped to 0..32 |
| `FrqO` | FreqOffset | Hz, fixed detune (UI curve `SigmoidCentreZero<10>`) |

With `Rat=0` and only `FrqO`, the operator works as an LFO **[M]**.

### Connections `FM/Con/Oxy` (`cConnection`)

| Element | Connection |
|---|---|
| `O11` | OP1 → OP1 (feedback) |
| `O12` | OP1 → OP2 |
| `O13` | OP1 → OP3 |
| `O1O` | OP1 → output |
| `O22` | OP2 → OP2 (feedback) |
| `O23` | OP2 → OP3 |
| `O2O` | OP2 → output |
| `O3O` | OP3 → output |

Each connection has `Amt` (0..1, internal `Amount`) and **its own envelope** `Env` with the same
format as [the voice envelopes](05-envelopes-lfos.md) **[P][M]**. The internal names are in the
binary as `OP1ToOP2`, `OP1Out`, etc. **[B]**.

The implied graph, with OP2→OP1 and OP3→anything missing:

```
OP1 ⟲ ─► OP2 ⟲ ─► OP3 ─► out
  │        │        ▲
  └────────┼────────┘
  └─► out  └─► out
```

Order of computation per sample: OP1, OP2, OP3 **[I]**. There are no cycles except
self-feedback, so it can be computed in a single pass.

### Implementation **[B]** (`0x100565478`)

- **Fixed-point engine**, DX style: 32-bit integer phases (`inc = f/sr · 2³²`).
- Each operator's frequency: `ratio · f_note` plus the fixed offset `FrqO` in Hz. With
  modulation (`FM OPn Freq`), `ratio = clamp(Rat·0.0001 + 10·m, 0, 32)` **[B]**. The table band is
  chosen with `ratio · f_note` (without `FrqO`) scaled to 44.1 kHz.
- **Waveforms in 256-sample int16 tables**, embedded in the binary, with **band-limited
  versions** chosen by the operator's frequency (normalized to 44.1 kHz). Pointer table per
  shape at `0x100c2e110`, with 15 shapes. Each pointer leads to a list of
  `{float max_hz, u32, u64 table}`, ending in `FLT_MAX` and pointing to a silent table **[B]**.
  The tables have 257 int16 (with a guard sample). The engine's order is not the UI's. It was
  identified from each table's harmonics and slope **[B][I]**: 0 Sine, 1 Parabola (odd at
  1/k³), 2 Triangle (1/k²), 3 Saw Up, 4 Saw Down, 5 Square, 6 Half Wave, 7 Full Wave,
  8..11 Sine 1+2..1+5, 12..14 Rez 2/3/5 (peak at harmonic 2, 3 or 5). The last band of the 14
  non-sine shapes is the sine table (≤ 18 kHz).
  [`firmware/tools/fmwaves2h.py`](../../firmware/tools/fmwaves2h.py) extracts them.

  | Shape | Cutoffs (Hz at 44.1 kHz) |
  |---|---|
  | Sine | a single table up to 18000; silence above |
  | The other 14 | 400, 800, 1000, 1200, 1600, 3000, 18000; silence above |

  So a saw or square operator does not alias on high notes.
- Each of the 8 connections has its envelope, which starts, releases or retriggers with the
  voice. The amount (below) is **converted to an integer** and interpolated sample by sample
  within the block. There is also a limiter of **±6 units per block** on a stored value, whose
  exact use is **[?]**.
- **Per-sample render** (`0x1002bf5cc`, one of several versions specialized by which
  connections are active):

  ```text
  o1 = T1[ φ1 + fb1·avg1 ]                   // read with linear interpolation (16 bits)
  o2 = T2[ φ2 + fb2·avg2 + m12·o1' ]         // o1', o2': outputs of the previous sample
  o3 = T3[ φ3 + m13·o1' + m23·o2' ]
  avg1 = (o1 + avg1) >> 1;  avg2 = (o2 + avg2) >> 1   // averaged feedback, like the DX7
  out  = (a1·o1 + a2·o2 + a3·o3) · 2.4651e-9 // a·: levels O1O, O2O, O3O
  ```

  The outputs are int16 and the amounts integers, so the phase modulation is
  `amount × output` in 32-bit phase units, with 32-bit multiplications that wrap around. Table
  index `φ >> 24`; fraction `(φ >> 8) & 0xffff`. The modulators use the output of the previous
  sample: the code stores `o1`/`o2` and reads them before updating them (`0x1002bf644`).
- **Amount of each connection [B]** (`0x100565478`):

  ```text
  e   = 1 − Levl + Levl · envelope       // Levl of the connection's Env = depth
  a   = clamp(e × velocity_scale × (Amt + mod), 0, 1.785)
  int = K · a²                      // square law
  K   = 272625  (O11, O12, O13)     // modulators from OP1
        262140  (O22, O23)          // modulators from OP2
        16384   (O1O, O2O, O3O)     // output levels, Q15: 16384 = 0.5
  ```

  What NS2 renders show contradicts this reading: with `Levl = 0` (the value of almost every
  factory connection) **the envelope acts in full** **[P]**. House Pluck Bass, with `O3O` at
  decay 1.5 s and sustain 0, reaches digital silence in 1.5 s with the key held. The port uses
  `e = Levl + (1 − Levl) · envelope`. With that, FM Stoned Keys, House Pluck Bass, Hang Drum and
  Ef Em Crystal Keys match NS2 in level, envelope and harmonics. The outputs `O1O..O3O` with
  intermediate amounts (0.25, 0.5, two carriers at once) and Saw Up/Saw Down operators as
  carriers match NS2 **[P]**. Feedback on a Saw Down operator (`O11` 0.5) matches NS2 harmonic by
  harmonic; the level difference measured earlier was its DC offset, which NS2 removes
  ([04](04-filters.md#very-low-cutoff-and-dc-blocking-p)) **[P]**.
  Tests with `O3O` at decay 0.6 s and sustain 0 confirm the formula with `Levl` 0.5 and 1 (exact
  match) **[P]**. With `Levl` 0, NS2 decays the same way up to 100 ms, but then a −35 to −40 dB
  tail remains that the port brings down to −56. It may be the ±6-units-per-block limiter or
  the quantization of the integer amount **[?]**.

  With `Amt = 1` a modulating connection shifts the phase by up to ~2.08 cycles, that is a
  **modulation index of ~13 rad**, on the order of a DX7's maximum **[I]** (this assumes there is
  no extra shift in the render). With modulation `a` can reach 1.785.
- The internal order of the 8 connections is O11, O12, O13, O22, O23, O1O, O2O, O3O; in the XML
  they appear in alphabetical order (O11, O12, O13, O1O, O22, O23, O2O, O3O) **[B]**.

## NanoSaw (`NS` → `cOscTypeNanoSaw`)

"Several layered and detuned saws" **[M]**. It is a supersaw. Decompiled at `0x10046d1c4`
**[B]**.

| Attr | Internal | Observed range | What it does **[B]** |
|---|---|---|---|
| `Mix` | Mix | 0.18..1 | Level of the side saws against the centre one |
| `FlOf` | Falloff | 0..1 | How much the more detuned pairs drop |
| `Dtn` | Detune | 0..0.75 | Maximum detune, with a piecewise curve |
| `Sprd` | Spread | 0.3..0.9 | Distribution of the detune among the 3 pairs (not stereo) |

**Algorithm:**

```text
d  = clamp(Dtn + mod_detune, 0, 1)
D  = 0.5 · (d < 0.6 ? d/6 : 2.5·d − 1.4)      // continuous curve with a break at 0.6
s  = 2.25 − 2·Sprd²
a3 = D                     // outer pair
a2 = D · 1.5^(−s)          // middle pair
a1 = D · 3^(−s)            // inner pair
frequencies: f, f·(1 ± a1), f·(1 ± a2), f·(1 ± a3)        // 7 saws

m  = clamp(Mix + mod_mix, 0, 1)
S  = k · 1.1 · (0.75 + 0.25·FlOf) · m
gains: centre = k·(1 − m/2)
       inner  = S·(1 − FlOf/18)
       middle = S·(1 − 2·FlOf/9)
       outer  = S·(1 − FlOf/2)
```

- `k` is the normalization of the antialiasing method (inverse of `f/sr`, see below).
- **Measured level [P]** (tests `zn01`–`zn05`): with these gains the port came out 1.1 dB louder
  with `Mix` 0 and 6.2 dB louder with `Mix` 1. In NS2 the centre saw plays at 0.88 and the six
  side saws at 0.52 of what the formula gives; with these factors the 5 tests are within ±1 dB.
- **Antialiasing: DPW** (Differentiated Parabolic Wave). Each saw has a 32-bit integer phase,
  and the previous state is initialized as `(phase/2³¹)²`, the parabola the method
  differentiates **[B]**.
- **Random start phases** on every note-on (LCG `x·31415821 + 1`, the same as the LFOs'). The
  centre saw always starts at `0x7FFFFFFF` **[B]**.
- Internally there are **two groups of 4 saws**: centre + the three detuned down, and centre +
  the three detuned up. Probably one per channel, with `Widt` controlling the stereo mix **[I]**.

## Noise (`Nois` → `cOscTypeNoise`)

| Attr | Internal | Values |
|---|---|---|
| `Type` | Type | 0 `White`, 1 `Pink`, 2 `Brown`, 3 `Low Passed`, 4 `High Passed`, 5 `Band Passed`, 6 `Notched` |
| `Deci` | Decimate | 0..1, sample-rate reduction **that follows the note** |
| `Cut` | Cutoff | 0..1, types 3..6 only |
| `Q` | Q | 0..1, types 3..6 only |

**Implementation [B]** (`0x10056c920`, `0x10056cd20` mono and `0x10056ca30` stereo):

- **Generator**: two 32-bit integers, `x' = (x >> 2) + 4x + y` and `y' = x' + y`, and the output
  is converted to float. It is a cheap additive generator, not the LFOs' LCG.
- **Decimation (sample & hold)**: a 16-bit fractional phase accumulator decides when to output
  a new sample; meanwhile it repeats the previous one. The rate is

  ```text
  d    = clamp(Deci + mod, 0, 1)
  rate = 2^((pitch − 60 − 128·d) / 12)      // new samples per sample, ≤ 1
  ```

  With `Deci = 0` and the note at C4 or above there is no decimation; below C4 the noise is
  decimated in proportion to the note, so the noise ends up "tuned". `Deci = 1` lowers the rate
  by 128 semitones (about 10.7 octaves).
- **Pink**: sum of 3 one-pole filters over white noise (Paul Kellet's "economy pink" style)
  **[B][I]**.
- **Brown**: a bounded random walk; if the next step goes out of range, it stays at the
  previous value **[B]**.
- **Low/High/Band Passed and Notched**: go through a filter of the **biquad family** (the same
  designers as the `-D` filters) **[B]**:

  ```text
  f = 20 · 1000^(Cut + mod)                  // 20 Hz .. 20 kHz
  q = 0.25 + k · (Q + mod)²                  // k = 15.75; in Notched, k = 3.75
  if f < 40 Hz: q *= 0.025 · f
  ```
- With stereo width (`Widt` > 0) two independent noise channels are generated with SIMD
  **[B][I]**.

## Sample (`Samp` → `Zones` → `Zone*`)

Up to **24 zones**, with at most 2 overlapping (crossfade) **[M]**.

| `Zone` attr | Values |
|---|---|
| `File` | Path. `/AppRes/...` = app resource; a bare name = file in the preset folder; `""` = empty |
| `Root` | Root MIDI note |
| `Lo` / `Hi` | MIDI note range |
| `Tune` | Fine tuning (±0.15 observed). The render uses `Root − Tune` as the effective root **[B]**: it raises the zone by `Tune` semitones **[I]** |
| `Vol` | Linear gain |
| `Pan` | 0..1 |
| `Dir` | `Forward`, `Fwd-Rev` |
| `Loop` | `Off`, `On`, `Spectral` |

- **Spectral loop**: generates a new sample from the spectrum of the original, so it can loop
  without clicks **[M]**. The binary has windows (`Hanning`, `Gaussian`, `BlackmanH`,
  `Welch`…) and the variants `Spectral (Entire Sample)` / `Spectral (Sustain Loop)` **[B]**. It
  is the most expensive block and the first candidate to leave out of a port.
- The loop points are not in the zone: they come from the WAV's `smpl` metadata (the string
  `Smpl` is in the binary) **[I]**. The WAVs of a user preset with samples (RhodesMK2) carry one
  forward loop per file in `smpl`, with an inclusive end **[P]**.
- Own modulation destinations: `Sample Start`, `Sample Loop Start`, `Sample Loop Len`,
  `Sample Zone Shift` **[B]**.

### How it plays **[B]**

Decompiled from `0x10056fddc` (note-on), `0x10056ff7c` (zone start) and `0x10057020c` (render).

- **Zone choice**: the note (plus `Sample Zone Shift`·100), clamped to 0..127, is compared with
  each zone's `Lo..Hi` in order. **The first two zones** that contain it play, each in its own
  player **[B]**. That the value is the MIDI key and not the pitch with `Trn` is **[I]**.
- **Crossfade between overlapping zones**: a zone's gain is computed against the neighbouring
  zone with `(note + 0.5 − Lo)/(Hi_neighbour − Lo + 1)` (going up) or
  `(Hi + 0.5 − note)/(Hi − Lo_neighbour + 1)` (going down), clamped to 0..1 **[B]**. Outside the
  overlap the gain is 1.
- **Read speed**: `2^((pitch − (Root − Tune))/12)` times the ratio between the file's sample
  rate and the engine's, with the oscillator's `pitch` from the
  [common part](#common-part-pitch-and-dispatch-by-type-b) **[B][I]**.
- **Gain**: zone crossfade · `Vol` · zone pan. The pan is constant power (`sin`/`cos` of
  `Pan·π/2`); with a mono zone at the centre it applies `0.7071` **[B]**.
- `Dir` and the `Loop` type choose the player mode (`0x10059f774`); `Spectral` starts without
  a direction of its own **[B]**. The player details (interpolation, `Fwd-Rev`) are **[?]**.

A user preset confirms the `Root` convention **[P]**: in RhodesMK2 the samples sound ~1.5
semitones below their `Root` (autocorrelation of the WAV) and the preset compensates with `Trn`
1 and `Dtn` ≈ 0.47 on all three oscillators. With the formula above, the port plays `ROM C4`
(Root 72) on note 72 at 522.9 Hz (C5 = 523.25 Hz).

---

## Velocity layers and splits (`Osc` → `cOscTriple`) **[B]**

Decompiled from `0x1005a6178`, called by the note-on (`0x1005a60b8`) and by legato
(`0x1005a6450`, only when the velocity changed). It computes a gain `g0..g2` per oscillator from
the velocity `v` (0..1; that it is `vel/127` is **[I]**). An oscillator with gain 0 does not start
or render; the gain multiplies the oscillator's level. A crossfade with `LX < 0.01` is a hard
cut.

`Type` is the `VelSplitType` enum: `Layer` (0), `Split 2` (1), `Split 3` (2). The parameters are
in [02](02-preset-format.md#osc--cosctriple-velocity-layers-and-splits).

```text
Layer:    g0 = 1
          gk = 1                         if v ≥ LLk          (k = 1, 2; Osc 2 and Osc 3)
             = (v − t)/(LLk − t)         if v > t,  t = LLk·(1 − LXk)
             = 0                         otherwise
Split 2:  g2 = 1                                             (Osc 3 does not take part)
          v ≤ S2L1:  g0 = 1, g1 = 0
          otherwise: e = S2L1 + LX1·(1 − S2L1)
                     v < e:  g0 = (e − v)/(e − S2L1), g1 = 1 − g0
                     else:   g0 = 0, g1 = 1
Split 3:  v ≤ S3L1:         g = (1, 0, 0)
          v ≤ S3L2:         e = S3L1 + LX1·(S3L2 − S3L1)
                            v < e:  g0 = (e − v)/(e − S3L1), g1 = 1 − g0, g2 = 0
                            else:   g = (0, 1, 0)
          v > S3L2:         e = S3L2 + LX2·(1 − S3L2)
                            v < e:  g2 = (v − S3L2)/(e − S3L2), g1 = 1 − g2, g0 = 0
                            else:   g = (0, 0, 1)
```

- `LX1`/`LX2` are the **crossfade width as a fraction of the zone above**: the crossfade between
  Osc 1 and Osc 2 takes the start of the middle zone, and the one from Osc 2 to Osc 3 the start
  of the top zone. The bottom zone never mixes.
- In `Layer`, `LL1`/`LL2` are the velocity from which Osc 2/Osc 3 come in, with a fade-in of
  width `LX·LL` below it. The defaults (`LL = 0`) make all three always play.
- Example (RhodesMK2, `Split 3`, `S3L1` 0.353, `S3L2` 0.8745, `LX1` 1, `LX2` 0.0325): Osc 1 alone
  up to 0.353; from 0.353 to 0.8745 a linear crossfade Osc 1 → Osc 2 (at v = 0.5, 0.72/0.28);
  from 0.8745 to 0.8786 Osc 2 → Osc 3; above, Osc 3 alone.
