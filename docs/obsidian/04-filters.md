# Filters

- `F1`, `F2`: the two filters of each voice.
- `F3`: the stereo global filter, applied to the voice sum before the FX **[M][I]**. That F3 is
  the global one follows from its `KeyT` being 1 in all 312 presets (it has no note to follow) and
  from the XML having 3 blocks for "2 per voice + 1 global".

All three use the same class, `Audio::cSynthFilter` **[B]**: a multimode filter whose `Type` also
selects waveshapers, comb, formant and lo-fi.

## Parameters

| Attr | Internal | Range | UI format **[B]** | Notes |
|---|---|---|---|---|
| `En` | Enable | `On`/`Off` | | |
| `Type` | Type | see list | | |
| `CutO` | Cutoff | 0..1 | `cUIParamFormatCutoffFreq` | **`Hz = 20 · 900^CutO`** (20 Hz..18 kHz), see below **[B]** |
| `Q` | Q | 0..1 | `cUIParamFormatQ` | Normalized resonance; the mapping depends on the family, see below **[B]** |
| `Drv` | Drive | 0..1 | % | Saturator of the ladder (`-A`) and gain of the waveshapers; the `-D` and EQ types ignore it **[B]** |
| `KeyT` | KeyTrack | 0..2 | 0..200 % | `Hz *= (f_note / 130.8128)^KeyT`: neutral at C3 (MIDI 48) **[B]** |
| `OGn` | OutputGain | 0..2 | dB, curve `Power<200>` | Linear gain |
| `Pan` | OutputPan | 0..1 | pan | |
| `SGan` | ShelfGain | -24..24 | dB | EQ types only |
| `FVwl` | FormantVowel | 0..1 | % | Formant only: vowel morph |
| `CMix` | CombMix | 0..1 | % | Mix for Comb and Lo-Fi ("Comb/Lo-Fi Mix") |
| `WSGn` | WSGain | 0..1 | % | Waveshapers only; in NS2 renders it changes nothing **[P]** |

The values of parameters that do not apply to the chosen type are stored anyway **[P]**.

## Cutoff: formula **[B]**

Decompiled from the UI format function (`0x1000a2c94` and its twins for Noise and Slate) and
from the filter's coefficient update (`0x10055a290`):

```text
K      = log2(900)                      // static initializer: 1.442695 × ln(900) = 9.81378
hz     = 20 · 2^(K · (CutO + mod))      // = 20 · 900^(CutO + mod)
if note >= 0 and KeyT > 1e-6:
    hz *= (noteFreq[note] / 130.8128)^KeyT     // 130.8128 Hz = C3, MIDI 48
hz     = min(hz, 18000)
```

- `CutO = 0.5` gives 600 Hz, `0.25` gives 109.5 Hz and `0.75` gives 3286 Hz.
- Cutoff modulation is added **in the normalized domain**, before the exponential. That is why a
  given depth always moves the same number of octaves: 1.0 of modulation = log2(900) ≈ 9.8
  octaves **[B]**.
- The other filter parameters are added to their modulation and clamped to their range
  (`fminnm`/`fmaxnm`) before use **[B]**.
- The parameters go through an internal slew to avoid jumps (constant `30 ·` sample rate in the
  update) **[B]**. The exact slew constant is **[?]**.

## Internal structures **[B]**

The filter patch (`cSynthFilter::cPatch`) stores the floats in this order, the same as the XML.
Indices in 32-bit words:

```text
[4] En  [5] Type  [6] CutO  [7] Q  [8] Drv  [9] SGan  [10] FVwl  [11] CMix
[12] WSGn  [13] KeyT  [14] OGn  [15] Pan
```

Modulation reaches the filter as a vector of 9 floats, in the order of the `DstP`s, and each is
added to its parameter and clamped to its range:

| # | DstP | How it applies |
|---|---|---|
| 0 | Cutoff | `CutO + m` (normalized) |
| 1 | Cutoff Slow | `CutO + slew(m)`, with a slope limiter proportional to 30·Δt |
| 2 | Q | `Q + m` |
| 3 | Drive | `clamp(Drv + m, 0, 1)` |
| 4 | (Shelf Gain) | `clamp(SGan + 24·m, -24, 24)` dB |
| 5 | Formant Vowel | `clamp(FVwl + m, 0, 1)` |
| 6 | Comb/Lo-Fi Mix | `clamp(CMix + m, 0, 1)` |
| 7 | Output Gain | `clamp(OGn + 2·m, 0, 2)` |
| 8 | Output Pan | `clamp(Pan + m, 0, 1)` |

In every case, modulation `m = 1` equals the parameter's **maximum value** (1, 2 or 24). See
[06-modulation.md](06-modulation.md#scales).

## Q and resonance **[B]**

`q = clamp(Q + mod, 0, 1)`, smoothed. After that it depends on the family:

**Biquad (`-D`, at `0x1005399d8`)**:

```text
q' = q · min(1, f / 40 Hz)        // less resonance below 40 Hz
Q_biquad = 0.5 + 5.5 · q'²        // LP/HP 12-D: from 0.5 to 6.0
```

The designer's types are 1 = 2-pole LP, 2 = 2-pole HP, 3 = 1-pole LP, 4 = 1-pole HP, 8 = shelf
and 12 = peak. For EQ Peak, `Q = 0.25 + 7.75·q²` and the gain is `SGan` in dB. The port uses
RBJ's peaking filter. **EQ Shelf [B][P]** (designer `0x10053aa74`, called from `0x10055a81c`):

```text
A    = 10^(SGan/40)
S    = clamp(Q², 0.01, 1)                    // slope: with Q 0 the transition spans decades
RBJ low shelf with that S, and the b's multiplied by 1/A
     → lows at +SGan/2 dB and highs at −SGan/2 dB: a tilt EQ
```

The caller computes the 1/A with a fast bit-level exponential:
`bits = SGan·(−696658.5) + 1065292416 ≈ float(10^(−SGan/40))`.

This was measured against NS2 with a saw:
- +12 dB with Q 0: from 0 dB to −6 dB.
- −12 dB with Q 0: from 0 dB to +6 dB.
- +24 dB with Q 1 and cutoff 0.5: from +12 dB to −12 dB, around 600 Hz.

With this model, the port matches in level and bands.

**Level of the ladder's saturator [P]**: the voice feeds the filters at half level. For Drive
to saturate as in NS2, the ladder gets twice the input and its output is halved. With that
factor, the tests at Drive 0.3, 0.6 and 1.0 (oscillator at level 1 and 0.25) match NS2 in level
and bands. Without it, at Drive 1 the port came out up to 5.6 dB louder and brighter.

**Output Gain comes after the saturator [P]**: a saw through LP 12-A at Drive 0.894 with `OGn`
0.5 plays 6 dB under `OGn` 1 in NS2 (test `zu12`). The port used to scale the ladder's input by
it, so the saturation ate most of the cut and it came out 5.3 dB loud. Bell Evolve, whose knob
drives the filter and lowers its Output Gain, was 4.3 dB loud for this reason.

**F1 disabled (`En=Off`)** does not filter: in the port it is `kFilterOff`, which only applies
the gain.

**Analog model (`-A`, setup at `0x10054b098`)**: a **4-pole multimode ladder**. The output is a
linear combination of the input and the 4 one-pole stages, as in the Oberheim Xpander:

```text
fn   = clamp(f / sr, 0, 0.25)
d    = clamp(Drv + m, 0, 1)                  // Drive, smoothed in the filter state (+0x10)
sat0 = 4.9·d³ + 0.1                          // gain before the saturator
sat1 = (14.5·d³ + 1.5) / sat0                // gain after it: the total is 14.5·d³ + 1.5
q' = q;  if f < 40 Hz: q' *= 0.5 + 0.0125·f
q' *= per-mode compensation (below)
if q' ≥ 0.5: q' = 0.5 + 0.5·sqrt(2q' − 1)    // stretches the upper half
w  = 2π·fn
c  = fn·(0.33049 + 0.13697·w² − 3.6557·fn)
G  = fn·(6.21533 + 0.86771·w²) + w·fn·(−2.72816 − 0.12692·w²)
k  = q' · (3.0087 + 3·c)                     // Notch: k = q'
```

`0x10055a290` passes the smoothed Drive to the ladder setup (`0x10054b098`) as its fourth float
**[B]**. There is also a limit `k ≤ 3/(sat0·sat1)` that is skipped when the filter's byte `+0x1c`
is 1 **[B]**. The constructor (`0x10055a1e8`) sets it to 1, but without the limit the ladder
self-oscillates with moderate Drive and Q (Drive 0.55 and Q 0.5 already give ×7 across the
spectrum), so Obsidian probably sets it to 0. The port applies the limit **[I]**.

Mode table (`0x1009c3450`, 11 entries of 32 bytes). Output coefficients `[x, y1, y2, y3, y4]`:

| Mode | Type | Gain | x | y1 | y2 | y3 | y4 | q' compensation |
|---|---|---|---|---|---|---|---|---|
| 1 | LP 12-A | 1 | 1 + 0.8k | 0 | 0.75 | 0 | 0 | `×(0.8 + 0.2d)`, and `÷ sat0` if > 1 |
| 3 | LP 24-A | 1 | 1 + 0.8k | 0 | 0 | 0 | 0.75 | `÷ sat0` if > 1 |
| 5 | BP 6-A | 0.75 | 0 | 2 | −2 | 0 | 0 | `×(0.82 − 0.22d)` |
| 6 | BP 12-A | 0.75 | 0 | 0 | 4 | −8 | 4 | `×(0.82 − 0.22d)` |
| 7 | HP 12-A | 0.7 | 1 | −2 | 1 | 0 | 0 | `×0.8`, and `÷ sat0` if > 1 |
| 8 | HP 24-A | 0.7 | 1 | −4 | 6 | −4 | 1 | `×(0.83 − 0.33d)`, and `÷ sat0` if > 1 |
| 9 | Notch 6-A | 0.7 | 1 | −2 | 2 | 0 | 0 | — |
| 10 | Notch 12-A | 0.7 | 1 | −4 | 8 | −8 | 4 | — |

- The Notch modes have no feedback: `k = q'` scales the sum of the stages (below).
- With high Drive (`sat0 > 1`, that is `d > 0.57`) the LP and HP resonance drops by `1/sat0`.
- Modes 2 and 4 are LP 12/24 without the `0.8k` term, and no Obsidian type uses them.
- The HP, BP and Notch coefficients are exactly the binomials of `(1 − L)^n`, which is what one
  expects from a multimode ladder. The `x·(1 + 0.8k)` term of the LP modes is a bass
  compensation: it adds back part of the input so the resonance does not "thin out" the sound
  **[I]**.
- **Per-sample process [B]** (`0x10054b3f8` dispatches; `0x100556498` renders modes 1..4,
  `0x1005565ac` the Notch modes and `0x1005566c8` the rest):

  ```text
  u  = (LP: (1 + 0.8k)·x;  others: x) − k·y4         // Notch: u = x
  v  = clamp(sat0·u, −1, 1)
  s  = sat1·(v − v³/3)                                 // cubic saturator
  yi = (1 − G)·yi + G·(0.769·in + 0.231·in_prev)       // 4 stages; in = s, y1, y2, y3
  LP:     out = c2·y2 + c4·y4
  Notch:  out = cx·s + k·(c1·y1 + c2·y2 + c3·y3 + c4·y4)
  others: out = cx·s + c1·y1 + c2·y2 + c3·y3 + c4·y4
  ```

  `ci` are the mode table's coefficients times the mode's gain. With Drive 0 the saturator is
  almost linear up to `|u| ≈ 10` and the small-signal gain is 1.5. With Drive 1, the gain is 16
  (+24 dB) and the saturator's output clips at `±2.13`. `G`, `k` and `cx` are **linearly
  interpolated sample by sample** between the previous block and the current one.
- **Several voices are processed at once with SIMD (NEON)**: there are renders for 2 and for 4
  voices in parallel, and three families by mode (≤4, ≤10 and the rest). An ESP32 does not need
  this: one voice at a time is enough.

## Types (`Type`)

The full list from the binary, in string-table order **[B]**, with the total use across
F1+F2+F3 **[P]**:

| Family | Types | Use |
|---|---|---|
| Low-pass | `LP 6-D`, `LP 12-D`, `LP 12-A`, `LP 24-A` | 15, 34, 465, 102 |
| High-pass | `HP 6-D`, `HP 12-D`, `HP 12-A`, `HP 24-A` | 0, 2, 26, 24 |
| Band-pass | `BP 6-A`, `BP 12-A` | 5, 14 |
| Notch | `Notch 6-A`, `Notch 12-A` | 1, 12 |
| EQ | `EQ Peak`, `EQ Shelf` | 66, 29 |
| Formant | `Formant-M`, `Formant-F` (male/female voice) | 14, 11 |
| Comb | `Comb Pos`, `Comb Neg` | 5, 13 |
| Waveshaper | `WS Tube`, `WS Soft`, `WS Hard`, `WS Enhance`, `WS Sine`, `WS Tri` | 19, 29, 23, 4, 9, 1 |
| Lo-Fi | `Lo-Fi` | 13 |

Internal code of each type **[B]** (table `{u64 code, char* name}` at `0x100c2d4e0`, with 25
entries). The high byte is the family and the low byte the variant:

| Code | Type | Code | Type |
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

The function that computes the filter's response (`0x10055b0d8`) dispatches to two different
coefficient designers **[B]**:

- `0x10053bd3c` for `LP 6-D`, `LP 12-D`, `HP 6-D`, `HP 12-D`, `EQ Peak` and `EQ Shelf`. This is
  the "-D" family, a classic digital design (biquad) **[I]**.
- `0x10054b5fc` for the `-A` types: LP 12-A, HP 12-A, HP 24-A, BP 6/12-A and Notch 6/12-A. This is
  the analog model. The `LP 24-A` branch does not read clearly in the pseudocode **[?]**.
- Formant (`0x500/0x501`) and Comb (`0x600/0x601`) have their own functions (`0x100549a90`,
  `0x1005419b8`).

What the code says about `-A`/`-D` confirms that they are two implementation families; which
topology each uses is **[?]**.

Naming:
- The number is the slope in dB/octave: 6 = 1 pole, 12 = 2, 24 = 4.
- `-A` and `-D` probably stand for "Analog" (non-linear modelling, ladder/OTA style with
  saturation) and "Digital" (clean biquad/SVF) **[I]**. All the 6 dB LP/HP types are `-D`, and the
  24 dB types only exist as `-A`.

## Formant, waveshapers and Lo-Fi **[B]**

**Formant-M / Formant-F** (response at `0x100549a90`): 4 parallel band-pass resonators whose
frequencies come from a vowel table (`0x1009c312c`), interpolated by `FVwl`. The table is
**Peterson & Barney (1952)**, with 10 American English vowels. Columns: F0, F1, F2, F3 in Hz.

| # | Male (Formant-M) | # | Female (Formant-F) |
|---|---|---|---|
| 0 | 136, 266, 2293, 2937 (/i/) | 10 | 231, 310, 2783, 3312 (/i/) |
| 1 | 135, 392, 1993, 2568 (/ɪ/) | 11 | 232, 441, 2474, 3063 (/ɪ/) |
| 2 | 128, 526, 1854, 2481 (/ɛ/) | … | the rest of the female table follows |
| 3 | 125, 664, 1727, 2420 (/æ/) | | |
| 4 | 129, 631, 1192, 2377 (/ʌ/) | | |
| 5 | 124, 718, 1090, 2442 (/ɑ/) | | |
| 6 | 127, 568, 836, 2403 (/ɔ/) | | |
| 7 | 136, 437, 1023, 2245 (/ʊ/) | | |
| 8 | 139, 307, 876, 2239 (/u/) | | |
| 9 | 132, 489, 1360, 1709 (/ɝ/) | | |

The values are the published ones, so a port can take them straight from the paper.

**Measured against NS2 [P]** (white noise through the filter, tests `zm01`–`zm16`):
- The four resonators sit at the vowel's **F0, F1, F2 and F3**, linearly interpolated between
  the 10 vowels of the table by `FVwl` (0..1 → vowel 0..9).
- **The frequencies scale with the cutoff**: `f = f_table · Hz_cutoff / 800`. With CutO 0.5
  (600 Hz) they land at 0.75 times the table, with 0.7 at ~2.9 times, and with CutO 1 they pile
  up at the ~14.4 kHz ceiling.
- Each resonator is a 2-pole band-pass with `Q_bp = 0.94·4^Q` and peak gain `Q_bp^1.5` (F0 at 0.7
  of that). With Q 0 the response is a +5 dB hump; with Q 0.5, peaks of +13 to +15 dB.
- As a global filter it responds the same as in the voice.

With this model (`syn::Formant`, 4 SVFs in parallel) the tests at CutO 0.5 are within ±0.4 dB in
level; at CutO 0.2–0.3 the port comes out 2–3 dB louder, and more at CutO 1. Open **[?]**: in
Dirty Saws (global Formant-M with `OGn` 0.25 and LFO → Formant Vowel) NS2 attenuates by ~7 dB
and the port by ~16 dB.

**WS Tube … WS Tri**: they reuse NanoStudio's **WaveShape** effect (`0x1000fd398`) with shape
1..6. Each shape is a **table of 1025 floats** generated at startup (`0x1000fcc94`): it starts
from a linear ramp from −1 to 1, applies the shape's curve (`0x1000fd848`, which uses `expf`,
`sinf` and `asinf` with its own constants), and then normalizes by the peak and subtracts the
centre value to remove DC. Curves over the ramp `x ∈ [−1, 1]` **[B]**, except where marked
**[I]**:

The curves, decompiled from `0x1000fd848` (the jump table at `0x1006db3ea` gives the order
1 Tube, 2 Soft, 3 Hard, 4 Enhance, 5 Sine, 6 Tri) **[B]**. `knee(a, e, r², c)` is
`c + sqrt(r² − (min(a, e) − e)²)`: a circular knee that joins the line to a flat top.

| Shape | Curve over `x ∈ [−1, 1]` |
|---|---|
| WS Tube | `y = 0.5·(x + 1.41)² − 1`; if `y > 0.9`: `knee(y, 1.1413, 0.11655, 0.6587)`; if `y < −0.3`: `1.3891 − sqrt(5.7077 − (min(−y, 1.9891) − 1.9891)²)`. Asymmetric |
| WS Soft | `1 − 2/(e^(clamp(18.984·x, ±10)) + 1)` = `tanh(9.49·x)` |
| WS Hard | `v = 15·x`; if `|v| ≤ 0.8`: `v`; otherwise `sign·knee(|v|, 1.2826, 0.46594, 0.31741)` |
| WS Enhance | if `|x| < 0.1`: `(10·|x|)²·x`. Then, for `y ≥ 0`: `v = 1.7065·y`, `knee(v, 1.7065, 2.912, −0.70648)` past 0.5; for `y < 0`: `v = 1.1413·y`, `−knee(|v|, 1.1413, 0.11655, 0.6587)` past 0.9 |
| WS Sine | `sin(13.5·x)` |
| WS Tri | `asin(sin(13.5·x))` |

Each table has 1025 points over the ramp (step 1/512), is divided by its peak and has its centre
value subtracted (`0x1000fcc94`).

**Gains** (`0x1000fd398`) **[B]**: `d = a + (1 − a)·Drive`, with `a = 0.15` for Tube and Enhance
and `0.02` for the rest. The table position is `512 + x·WSGn·1023·d`, and the output is
multiplied by `1 − 0.3·d` (×1.5 for Tube). That the first factor is `WSGn` and that the index is
centred on 512 is **[I]**. The full `WaveShape` effect (`0x100438424`) also oversamples ×4 or ×8
and mixes dry/wet; whether that happens inside Obsidian's filter is **[?]**.

**Measured model [P]** (27 tests against NS2, in
[`tools/ref/gen.py`](../../firmware/tools/ref/gen.py), `zw*`, `zu*`, `zq*`):
- **`WSGn` does nothing**: with 0.25, 0.5, 0.905 and 1, NS2 gives identical renders.
- **The cutoff is the dry/processed mix**: `y = (1 − c)·x + c·WS(x)`, with `c` the normalized
  cutoff plus its modulations (filter envelope included), without keytracking. At CutO 0 the
  signal comes out untouched, at 0.5 exactly half of each.
- The input gain is proportional to `d`, ~4.5 times that of the formula above with `WSGn` 0.5.
  The output follows `1 − 0.3·d` (×1.5 for Tube). `OGn` scales the output.
- **As the global filter** (F3), the input is ~5 times lower and the output ~5 times higher: the
  global waveshaper is almost linear and its gain grows with `d` (Soft at Drive 0.3 adds 17 dB).
- With this the tests match in level (±2.4 dB) and harmonics (±1.4 dB), except the Sine folder
  (4.7 dB off in the spectrum).

**`Drive` in the other types [B]**: the value goes through a slope limiter and is stored in the
filter state (`+0x10`, at `0x10055a290`). The ladder uses it as its saturator (see above), and so
does the Formant (`0x706` passes it to `0x1003d5884`). The biquads (`0x1005399d8`) and the EQs get
a fixed 1. The render families are 1 biquad (`0x10053b208`), 2 ladder (`0x10054b3f8`), 3 formant,
4 comb (`0x100541890`) and 5 (`0x1003d58a4`), according to `0x10055aca0`.

**Lo-Fi**: reuses NanoStudio's **LoFi** effect (`BitCrush`, `Decimate`, `Mix`); `CMix` is the
mix.

**Comb Pos / Comb Neg** **[B]**: setup at `0x10055aa90` (Pos) and `0x10055a874` (Neg), which call
`0x100541828`. The per-sample process is `0x100433730`. `0x1005419b8` only draws the curve in the
UI.

```text
f    = cutoff in Hz (same formula and keytrack as the other types, 20..18000)
D    = sr / f                                // samples, in 16.16; max 2047 (2048-sample buffer)
q    = clamp(Q + mod, 0, 1)
fb   = q / (q + 0.0618034·(1 − q))           // 0 if q < 1e-6; clamped to ±0.999
dry  = 1 − 0.3·CMix
wet  = dry·CMix
Neg: fb = −fb, wet = −wet

per sample:
  tap = delay[D]                             // linear interpolation
  h   = hp·h + (tap − tap_prev)              // DC blocker, hp = 1 − 2π·5/sr
  buf = in + fb·h
  out = dry·in + wet·h
```

- The constant 1.0618034 (`c` at `0x100d11d08`) is set by a static initializer.
- `D`, `dry`, `wet` and `fb` are linearly interpolated across the block. For the delay it
  crossfades between the read with the previous `D` and the new one.
- With `Q = 0` it is a feedforward-only comb: `0.7·in ∓ 0.7·in[n−D]` with `CMix = 1`.
- The port runs it in F3 and, per voice, in F1/F2 with the voice's keytracked cutoff. Physical
  String (a noise burst into a `Comb Pos` F1 with `Q` 1, a Karplus-Strong string) used to fall
  back to a self-oscillating LP 24-A; with the comb it matches NS2's spectrum **[P]**.

## Own modulation

Matrix destinations for a filter **[P][B]**: `Cutoff`, `Cutoff Slow`, `Q`, `Drive`,
`Output Gain`, `Output Pan`, `Formant Vowel`, `Comb/Lo-Fi Mix`.

`Cutoff Slow` is the same cutoff modulation but smoothed (with a slew) **[I]**. It is meant for
sources that jump, like the knobs or random.

`Dest` can be `Filter 1`, `Filter 2`, `Filter 1+2` (both at once) or `Global Filter`.

## Very low cutoff and DC blocking **[P]**

With the LP almost closed (CutO ≤ 0.1), the port matches NS2 in the steady-state harmonics. The
attack thump goes through the filter, and that thump depends on the saw's start phase
([03](03-oscillators.md)). A difference remains in that thump. NS2's chain has a **~5 Hz** DC
blocker (a one-pole high-pass): NS2's open saw run offline through a 4-pole LP at 39 Hz
reproduces its CutO 0.1 render exactly, while the port's saw only matches if that high-pass is
added. FM confirms it: a Saw Down operator with feedback (`O11` 0.5) has a large mean (−0.33
per voice in the port), and NS2 comes out centred, with the offset decaying at τ = 32 ms after
the note-on, that is a one-pole high-pass at 5 Hz. NS2's output carries no DC in any of the
test patches. It is not known whether it sits in the voice or at the output **[?]**; the port
applies it to the voice mix, after F3 and before the effects.

## Routing

See [01-architecture.md](01-architecture.md#filter-routing-rout-mp). Use: STEREO 121, PARALLEL 97,
SERIES 94 **[P]**.

## For a port

- The `LP 12-A` and `LP 24-A` models alone cover 70 % of F1's uses (220/312).
- A Cytomic/ZDF SVF gives 12 dB LP/HP/BP/Notch from a single core. Two in cascade give the 24 dB
  types. For the "-A" flavour, a `tanh` (or a polynomial approximation) at the input and in the
  feedback is enough.
- The waveshapers are memoryless functions: cheap, but they alias without oversampling.
