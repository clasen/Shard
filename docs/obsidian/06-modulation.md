# Modulation matrix

## Model

Each patch has **24 slots** (`Mod/C00..C23`) **[P]**. A slot is:

```
output = source × (multiplier ? macro_value : 1) × Dept/100   →   added to parameter DstP of Dest
```

The multiplicative form of the multiplier comes from the manual ("scales the connection's
value") **[M]**. How it is added to the target parameter (in the normalized 0..1 domain or in
real units, with or without clamping) is **[?]**.

- `Dept` goes from -100 to 100 **[P]**.
- Several slots can point at the same destination; they add up **[I]**.
- Empty slots (`None`) can be skipped. In the factory presets the average number of used slots
  is about 17 of 24 **[P]**, but many are macro connections that do not move during the note.

## Sources (`Src`)

Full list from the binary **[B]**, with use in the presets **[P]**:

| Source | Range **[I]** | Per voice | Use |
|---|---|---|---|
| `None` | — | — | 2223 |
| `Const One` | 1 | — | 0 |
| `Rand 1`, `Rand 2` | random value fixed per note | yes | 57, 37 |
| `Note` | normalized MIDI note | yes | 3 |
| `Velocity` | 0..1 | yes | 29 |
| `Release Vel` | 0..1 | yes | 0 |
| `Gate` | 0/1 | yes | 0 |
| `Amp Env`, `Filter Env`, `Pitch Env`, `Env 4`, `Env 5` | 0..1 (Bipolar ±1) | yes | 7, 342, 320, 78, 80 |
| `LFO 1` … `LFO 5` | depends on `OutT` | yes | 341, 355, 332, 192, 252 |
| `Pitch Wheel` | -1..1 | no | 304 |
| `Pitch Wheel +` / `Pitch Wheel -` | positive/negative half | no | 0 |
| `Knob 1` … `Knob 8` | 0..1 | no | 402, 397, 353, 306, 206, 154, 26, 23 |
| `Pad X`, `Pad Y` | 0..1 | no | 330, 339 |

The binary has no `Mod Wheel` or `Aftertouch` string near this table **[B]**. Most likely the
MIDI mod wheel is mapped to a knob through the MIDI CC table (`MIDI CONTROLLER MAP`) **[I]**.

## Multiplier (`Sca`)

`None` or a macro source (`Knob 1..8`, `Pad X`, `Pad Y`) **[P]**. It is used in 0.3 % of the
connections; it is a fine-grained tool for "more vibrato the further I turn the knob".

## Destinations (`Dest`) and parameters (`DstP`)

| Dest | Possible DstP **[P][B]** |
|---|---|
| `Osc All` / `Osc 1` / `Osc 2` / `Osc 3` | `Pitch Coarse`, `Pitch Fine`, `Level`, `Pan`, `Phase`, `Filter Mix`, `Ring Mod`, `Width`, `An Pulse Width`, `An Sync Ratio`, `An Sync Hardness`, `WT Position`, `WT Detune`, `PD Phase Dist`, `FM OP1->OP2`, `FM OP1->OP3`, `FM OP2->OP3`, `FM OP1 Freq`, `FM OP1 FB`, `FM OP1 Out`, `FM OP2 Freq`, `FM OP2 FB`, `FM OP2 Out`, `FM OP3 Freq`, `FM OP3 Out`, `NanoSaw Detune`, `NanoSaw Mix`, `Noise Decimate`, `Noise Cutoff`, `Noise Q`, `Sample Start`, `Sample Loop Start`, `Sample Loop Len`, `Sample Zone Shift` |
| `Filter 1` / `Filter 2` / `Filter 1+2` / `Global Filter` | `Cutoff`, `Cutoff Slow`, `Q`, `Drive`, `Output Gain`, `Output Pan`, `Formant Vowel`, `Comb/Lo-Fi Mix` |
| `Amp` | `Gain` |
| `Amp Env` / `Filter Env` / `Env 4` / `Env 5` | `Attack`, `Decay`, `Sustain`, `Release`, `Level`, `Delay`, `Retrigger` |
| `Pitch Env` | the above + `Attack Level`, `Release Level` |
| `LFO 1..5` | `Rate`, `Level`, `Curve`, `Warp`, `Loop Pattern` |
| `Voice` | `Unison Detune`, `Unison Stereo`, `Glide Time` |
| `FX` | `Multi-FX Mix`, `Multi-FX Rate`, `Multi-FX Depth`, `Multi-FX Delay`, `Multi-FX FB`, `Delay Mix`, `Delay Time`, `Delay FB`, `Delay EQ`, `Reverb Mix`, `Reverb Time` |

In practice `Osc All` is used almost only for `Pitch Coarse`: 818 of 829 uses **[P]**.

## Most used destinations (to prioritize in a port) **[P]**

| Connection | Uses |
|---|---|
| `Filter 1+2 / Cutoff` | 556 |
| `Osc All / Pitch Coarse` | 818 |
| `Amp Env / Attack` and `Release` | 250 + 242 |
| `Amp / Gain` | 232 |
| `Filter 1+2 / Q` | 170 |
| `Filter 1+2 / Cutoff Slow` | 161 |
| `Global Filter / Cutoff` | 137 |
| `FX / Delay Mix`, `Reverb Mix` | 109 + 108 |

## Scales

**What is decompiled [B]:**

- `Dept` is read from the XML as an **integer** (int reader, int32 range) into field `+0x20` of
  the connection (`0x1003ff868`). The other fields are `+0x10` Src, `+0x14` Dest, `+0x18` DstP
  and `+0x1c` Sca.
- Each destination receives the modulation already summed, as a vector of floats in the order
  of its `DstP`s, and **adds it to the parameter's value in that parameter's own units**. In the
  filter: `m = 1` moves the normalized cutoff by 1.0 (≈ 9.8 octaves), Q by 1.0, Output Gain by
  2.0 and Shelf Gain by 24 dB. That is, **`m = 1` equals the parameter's maximum value** (details
  in [04-filters.md](04-filters.md#internal-structures-b)).
- In the LFO, Rate modulation enters cubed: `Rt + 60·m³` (see
  [05-envelopes-lfos.md](05-envelopes-lfos.md)).

**What is missing [?]:**

- How `Dept` becomes `m` inside the matrix. The natural hypothesis is
  `m = Σ source × Dept/100 (× multiplier)` **[I]**: with it, `Filter Env → Cutoff` with
  `Dept=100` sweeps the whole range, which is what the presets sound like.
- The scale of **Pitch Coarse**. The "m = 1 → parameter maximum" rule would give ±36 semitones
  (the maximum of `Trn`), but the presets point to `Dept` = semitones (`m = 1` = 100 semitones)
  **[P][I]**: 304 factory presets bend with `Pitch Wheel → Osc All Pitch Coarse, Dept 2` (the
  usual ±2), and the hats' Pitch Envs rise an octave with `Dept 12`. The port uses 100. The
  constant was not found in the oscillator code.

**Render test to settle it (5 minutes in NS2):**

1. Init patch: one Analog Sine oscillator, no filter, no FX, note A3.
2. Connection `Pitch Env → Osc All / Pitch Coarse`, `Dept = 100`. EnvP in Bipolar with
   `SLv = 1`, a long attack (2 s, for example) and sustain 0.
3. Record the output and measure the frequency at the start: `semitones = 12·log2(f0/220)`.
4. Repeat with `Dept = 50` to confirm it is linear.

A typical factory pattern that helps calibrate **[P]**: slots 0..3 are usually
`Filter Env → Filter 1+2/Cutoff`, `Pitch Env → Osc All/Pitch Coarse`, and knobs 1..4 go to
Cutoff, Q, Attack and Release (matching the default macro names).
