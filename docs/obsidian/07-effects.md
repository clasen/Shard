# Effects

The chain has 3 units after the global filter, in a configurable order (`FX/@Ord`, see
[02-preset-format.md](02-preset-format.md#fx)) **[M][P]**. `FX/@En=0` bypasses the whole chain.
Each unit also has its own `En`.

They are "Simple" versions of NanoStudio's insert effects **[M][B]**.

## Multi-FX (`MFX` → `cEffectMultiFXSimple`)

| Attr | Internal | Values |
|---|---|---|
| `En` | Enable | 0/1 |
| `Type` | Type | `Chorus 1` `Chorus 2` `Ensemble 1` `Ensemble 2` `Flanger 1` `Flanger 2` `Phaser 1` `Phaser 2` |
| `Rat` | Rate | 0..1 |
| `Dep` | Depth | 0..1 |
| `Dly` | Delay | 0..1 (unused in Phaser **[I]**) |
| `FB` | Feedback | 0..1 |
| `Wid` | Width | 0..1 |
| `Mix` | DryWetMix | 0..1 |

Use: Chorus 1 162, Chorus 2 45, Phaser 2 32, Phaser 1 29, Ensemble 1 21, Ensemble 2 14,
Flanger 2 6, Flanger 1 3 **[P]**.

**Implementation [B]** (`0x10054da34`): two engines, a modulated delay line (chorus/flanger,
`0x1002d1a20`) and a phaser (`0x100249b18`):

| Type | Engine | Detail |
|---|---|---|
| Chorus 1 | modulated delay | mode 0 |
| Chorus 2 | modulated delay | mode 1 |
| Ensemble 1 | 2 × modulated delay | mode 1; the second with rate and delay ×0.707; output split into an extra channel (mix 0.5) |
| Ensemble 2 | 2 × modulated delay | mode 1; the second with rate and delay ×0.707; both summed |
| Flanger 1 | modulated delay | mode 2 |
| Flanger 2 | modulated delay | mode 3 |
| Phaser 1 | phaser | flag 0 |
| Phaser 2 | phaser | flag 1 |

- Parameters the delay engine receives: `Rat`, `Dep`, `Dly`, `FB` and `Mix` (+mod, clamped to
  0..1). The phaser receives `Rat`, `Dep`, `FB`, `Wid` and `Mix`.
- The inner engine works in fixed point (16-bit phase) and has a specialized version per mode.
  The delay and rate ranges of each mode are **[?]**.

**Measured against NS2 [P]** (tests `zf19`–`zf27`, `zf40`–`zf48`). In each 20 ms window the
output is fitted as `a·dry + b·dry(t − d)`:
- **Chorus 1** with `Mix` 1 sounds as dry 0.66 + wet 0.68, and with `Mix` 0.5 as dry 0.82 + wet
  0.33, that is `dry = 1 − Mix/3` and `wet = 2/3·Mix`. With `Dly` 0.5 the delay sweeps
  6.6–8.6 ms with a ~0.3 Hz LFO (`Rat` 0.25); with `Dep` 1 the sweep is ~2.5 times faster, and
  with `Rat` 0.75 the LFO is ~1.5 Hz. The mono sum is explained by a single delayed copy, so L
  and R follow the same LFO.
- **Flanger 1** mixes like the chorus; its delay sits at 2.2 ms with `Dly` 0.1 and at 5.2 ms with
  `Dly` 0.5, and `FB` feeds the wet signal back (a second copy at 2·d).
- **Phaser 1/2** with `Mix` 1 sounds as dry 0.8 + wet 0.8.
- Chorus 2 and the Ensembles give inverted copies or several at once: the fit does not resolve
  them **[?]**.

**In the port** (`syn::Chorus`): one modulated line per channel with a sine LFO:
`centre = 3 + 10·Dly` ms (flanger: `1.45 + 7.5·Dly`), `depth = ±3.4·Dep` ms (flanger ±3),
`rate = 0.12·e^(3.36·Rat)` Hz. Chorus 2 has twice the rate and 1.5 times the depth, and the
Ensembles add a second line with rate and delay ×0.707 **[I]**. `Wid` is unused. With this the
18 tests are within ±1 dB in level.

The full version (`cEffectMultiFX`) has separate parameters per type (`ChorusRate`,
`FlangerDelay`, `PhaserFeedback`…) and `BassBypass` **[B]**. The 1/2 variants of the simple
version are probably fixed presets of those internal parameters **[I]**.

## Delay (`Dly` → `cEffectDelaySimple`)

| Attr | Internal | Values |
|---|---|---|
| `En` | Enable | 0/1 |
| `Type` | Type | `Stereo`, `Cross 1:1` … `Cross 1:4`, `Pingpong 1:1` … `Pingpong 1:4` |
| `DTim` | DelayTime | seconds (0.0003..1.31 observed) when `TDen=sec` |
| `TNum` | TimeBeats | 1..4, numerator in tempo mode |
| `TDen` | TimeUnits | `sec` or a note denominator `1` `2` `4` `8` `12` `16` |
| `EQCt` | EQCut | 0..1, filter in the feedback path |
| `FB` | Feedback | 0..0.85 observed |
| `Mix` | DryWetMix | 0..1 |

- `1:N` is probably the time ratio between the L and R channels **[I]**.
- `Cross` feeds each channel into the other; `Pingpong` alternates the echoes between L and R
  **[I]**.
- The UI uses its own curve for the time (`cEden2DelayUI::cUIParamCurveDelayTime`) **[B]**.

### Implementation **[B]**

The simple Delay shares the processing of the full Delay (`cEffectDelay`, `0x1005af4a4`):

- **Two lines, L and R.** The time ratio comes from `RatioNum / RatioDen`: this is what the
  `1:1` … `1:4` types encode.
- **Type 2 (Pingpong)** uses a different routing setup from Stereo/Cross; the two routing flags
  are inverted depending on `Type == 2`.
- **Feedback**:
  - `Stereo` (type 0): each line feeds back into itself, with `FBBalance` (`b`) giving
    `FB·min(1, 2b)` to one and `FB·min(1, 2 − 2b)` to the other (a linear law, centred at 0.5).
    There is no cross feedback.
  - `Cross` and `Pingpong`: self-feedback is 0 and all of `FB` goes **across** to the other
    line.
- **Modulated time**: Delay Time modulation is quantized in steps of `1/64` and limited to 1..64
  subdivisions.
- **Feedback EQ**: `LowCut` and `HighCut` (in the simple version, `EQCut`) enter **squared** as
  the filter coefficient.

**Measured against NS2 [P]** (tests `zf02`–`zf08`, `zf50`, with short notes):
- **The dry signal does not drop**: it stays at 1 for any `Mix`. The first echo comes out at
  `≈1.1·Mix²` (−12 dB with 0.5, 0 dB with 1).
- `Delay Mix` from the matrix is added to that wet level, not to `Mix`: with `Mix` 0 and +0.5
  from a knob, the echo comes out at −6 dB.
- `FB` 0.5 drops 6 dB per echo, so the feedback is linear.
- **Cross and Pingpong 1:2** give the same result in mono: the input goes to the L line (`DTim`)
  and each echo bounces to the other line, of `DTim/2`. With 250 ms, the echoes fall at 250,
  375, 625, 750 ms…
- **`EQCt`** is a gentle shelf per pass: with 0.1 it cuts the highs above ~400 Hz by ~3 dB, and
  with 0.9 the lows below ~700 Hz by ~3 dB.
- 1/8 in tempo gives echoes every 240 ms, because the NS2 project on the Mac runs at 125 BPM.

**In the port** (`syn::Delay`): that model, with two lines of up to 6 s in PSRAM after the
Multi-FX and before the reverb. Tempo-synced times play at 120 BPM. The tests are within ±1 dB
per echo and per band.
- In the presets the use is Cross 1:2 (56), Stereo (45) and Pingpong 1:2 (22); the median
  `EQCt` is 0.5.

## Reverb (`Rev` → `cEffectReverbAlgoSimple`)

| Attr | Internal | Values |
|---|---|---|
| `En` | Enable | 0/1 |
| `Type` | Type | `Small Room 1` `Small Room 2` `Large Room 1` `Large Room 2` `Dense Room 1` `Dense Room 2` `Plate 1` `Plate 2` `Hall` |
| `Decy` | Time | 0..1 |
| `HDmp` | Damping | 0..1 |
| `LCut` | LoCut | 0..1 |
| `HCut` | HiCut | 0..1 |
| `Mix` | DryWetMix | 0..1 |

- The 9 types match the presets of the full Algoverb shipped in `Install.zip`
  (`Effects/Algoverb/*.Algoverb`: Dense room 1/2, Hall, Large room 1/2, Plate 1/2,
  Small room 1/2) **[P]**. Those presets are one-line XML files with the full parameters:

  ```xml
  <!-- Hall.Algoverb -->
  <Root FileVersion="1" V="1" Algo="Plate" PDel="0.35" PDMx="0.303334" HCut="0.2"
        LCut="0.453333" Decy="0.42" HDmp="0.188333" Dry="1" Wet="1" Widt="0.658333" />
  ```

  There are **4 base algorithms** (`Small Room`, `Large Room`, `Dense Room`, `Plate`), and the 9
  presets are variants of them **[P]**:

  | Preset | Algo | PDel | PDMx | Widt |
  |---|---|---|---|---|
  | `* 1` (Small/Large/Dense Room 1, Plate 1) | the one in the name | 0 | 1 | 0 |
  | `* 2` (Small/Large/Dense Room 2) | the one in the name | 0.0421 | 0.495 | 0.638 |
  | `Plate 2` | Plate | 0.0928 | 0.402 | 0.638 |
  | `Hall` | **Plate** | 0.35 | 0.303 | 0.658 |

  In all 9, `HCut=0.2`, `LCut=0.453`, `HDmp=0.188`, `Dry=Wet=1` and `Decy≈0.5` (0.42 in Hall).
  The hypothesis is that the simple reverb's `Type` picks the algorithm + `PDel`/`PDMx`/`Widt`
  from this table, and that the rest of its parameters come from the Obsidian preset **[I]**.
  For a port, implementing the 4 algorithms is enough.
- The full Algoverb also has `PDelay`, `PDelayMix` and `Width` **[B]**.
- `PDel` is in **seconds**: the pre-delay buffer is allocated for `0.35·sr` samples (rounded up
  to a power of 2), that is a 350 ms maximum, which is exactly the Hall's value **[B]**.

### Topology **[B]**

Both the simple and the full reverb use the internal **`Algoverb`** module (`0x1003edef8`). It
has a pre-delay, LoCut/HiCut filters (two biquads), the parameters `Size`, `Damping`, `Width`,
`Dry`, `Wet`, and **two engines**:

**Engine A: Freeverb-style comb bank** (`0x1001907cc`):

- 16 combs with a low-pass filter in the feedback (LBCF), 8 per channel. The base lengths are
  Freeverb's (Jezar), in samples at 44.1 kHz:

  ```text
  1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
  ```

  scaled by `sr/44100`. The second group of 8 is detuned by ±5 %, alternating (odd +5 %, even
  −5 %), to decorrelate the channels. Minimum 16 samples.
- It also allocates a buffer for the 4 Dattorro diffuser times (see below) plus two of 11.36 ms
  and 12.22 ms. They are probably input allpasses **[I]**.
- Damping: the combs' low-pass cutoff is at `22000 − 21200·Damping` Hz.

**Engine B: Dattorro plate** (`0x1002b0ab8`). This is the plate from J. Dattorro's paper
("Effect Design, Part 1", JAES 1997), with the original times (meant for 29761 Hz) expressed in
seconds:

| Element | Time | Samples at 29761 Hz (Dattorro) |
|---|---|---|
| Input diffusers 1..4 | 4.762, 3.583, 12.721, 9.297 ms | 142, 107, 379, 277 |
| Tank L: modulated allpass | 22.562 ms | 672 |
| Tank L: delay 1 | 149.433 ms | 4453 |
| Tank L: allpass 2 | 60.408 ms | 1800 |
| Tank L: delay 2 | 124.830 ms | 3720 |
| Tank R: modulated allpass | 30.454 ms | 908 |
| Tank R: delay 1 | 141.519 ms | 4217 |
| Tank R: allpass 2 | 89.138 ms | 2656 |
| Tank R: delay 2 | 106.145 ms | 3163 |
| Modulation excursion | 0.272 ms | ±8 |

- The input diffusers are scaled by one factor and the tank's by another; the most likely one is
  `Size` **[I]**.
- There are **two configurations** of engine B. They share the size function but have different
  constants: `{6.5625 ms, 0.6}` and `{2.734 ms, 0.75}`. They are probably a time and a diffusion
  coefficient **[I]**.
- There are 4 internal modulation LFOs (not just the paper's 2) **[B]**.

**Which algorithm uses which engine [B]** (process `0x1003ee7e4`; `Algo` enum: 0 Small Room,
1 Large Room, 2 Dense Room, 3 Plate):

| Algo | Engine | Variant |
|---|---|---|
| Small Room | A (Freeverb) | mode 0 |
| Large Room | A (Freeverb) | mode 1 |
| Dense Room | B (Dattorro) | configuration `{2.734 ms, 0.75}` |
| Plate | B (Dattorro) | configuration `{6.5625 ms, 0.6}` |

And "Hall" is Plate with a long pre-delay (see the preset table above).

**Rest of the process [B]:**

- Algoverb patch in XML order: `[4] Algo [5] PDel [6] PDMx [7] HCut [8] LCut [9] Decy [10] HDmp
  [11] Dry [12] Wet [13] Widt`.
- **Pre-delay**: `PDel·sr` samples. `PDMx` crossfades between the direct and the pre-delayed
  signal before it enters the reverb: `in + PDMx·(delayed − in)`.
- **Filters**, with counter-intuitive names:
  - `HCut` controls a **high-pass** at `20 + 12000·HCut³` Hz (0.2 → 116 Hz).
  - `LCut` controls a **low-pass** at `200 + 21000·(1 − LCut)³` Hz (0.453 → 3.6 kHz).

  Probably "HCut" means "cut of the lows" and "LCut" "cut of the highs", against convention, or
  the internal names are swapped relative to the UI's **[I]**.
- `Decy` and `HDmp` reach the engines as size/decay and damping.
- `Wet` and `Widt` enter squared when modulated; the stereo output is a mid/side mix driven by
  `Widt`.

**Measured against NS2 [P]** (tests `zf09`–`zf18`, `zf28`–`zf36`, `zf49`, with short notes):
- **Mix**: the dry signal stays at 1 up to `Mix` 0.5 and falls to 0 at `Mix` 1
  (`dry = min(1, 2·(1 − Mix))`); the wet signal grows linearly with `Mix`, also when `Mix` comes
  from the matrix.
- **Decay** (dB/s with `Decy` 0.1 / 0.5 / 0.9): Plate, Plate 2 and Hall 81 / 27 / 5.9; Dense
  Room 168 / 57 / 17; Small Room >400 / 61 / 18; Large Room >400 / 81 / 35. The Rooms with
  `Decy` 0.1 are barely audible.
- `HDmp` 0.8 lowers the tail by 7 to 14 dB above 1 kHz (Plate) and by up to 34 dB at 8 kHz
  (Small Room).
- `LCut` and `HCut` at 0.8 leave the wet signal 26 dB lower: this confirms a low-pass and a
  high-pass with the cutoffs above.
- The Hall is the Plate with pre-delay; Plate 2 and Room 2 barely differ from their version 1.

**In the port**: Plate, Plate 2, Hall and Dense Room use the Dattorro plate, and Small and Large
Room use Freeverb (8 combs and 4 allpasses per channel, with the plate's 4 diffusers at the
input; without them, the early echoes came out 6 dB louder than in NS2). `obs2preset.py` maps
`Decy` to each engine's feedback and sets the wet gain per type (tables `REVERB_DECAY` and
`REVERB_GAIN` at three `Decy` points, fitted against the renders), `HDmp` to the damping,
`HCut`/`LCut` to one-pole filters and the pre-delay from the preset table. The tests are within
±2 dB of wet level and ±3 dB/s of decay.

**For a port**: Freeverb and the Dattorro plate are public, documented algorithms. With the
lengths above, both families can be reproduced. On the ESP32 a single global instance (not per
voice) with the lines in PSRAM is the way to go: the plate's tank is about 0.36 s per side
(0.72 s in total), plus the diffusers and the pre-delay.

## For a port

- The reverb is the most expensive part. Freeverb (16 combs) and the Dattorro plate cover NS2's
  two engines; on the ESP32 you can start with the plate alone.
- The chorus and the delay are delay lines with linear interpolation; they are cheap.
- The effects process the mix, not each voice, so their cost does not grow with polyphony.
