# Envelopes and LFOs

## Envelopes (`cEnvelopeADSR`)

There are 5 per voice **[M][P]**:

| Element | Name in the matrix | Fixed connection |
|---|---|---|
| `EnvA` | Amp Env | Level and duration of the voice (VCA). The voice ends when this envelope reaches 0 |
| `EnvF` | Filter Env | — |
| `EnvP` | Pitch Env | — (`Bipolar` in 272 presets) |
| `Env4` | Env 4 | — |
| `Env5` | Env 5 | — |

The FM connections (`FM/Con/Oxy/Env`) use the same format **[P]**.

### Types (`Type`)

- `ADSR`: delay → attack → decay → (breakpoint → decay2) → sustain → release **[M][B][I]**.
- `AD`: no sustain, typical for percussion **[M]**. In the factory drum presets the AD amplitude
  has `S` 1 and a short `R`, so the decay must go to 0 and not to `S` **[P][I]**. The port plays
  it that way and ignores the note-off **[I]**.
- `Bipolar`: the output goes from -1 to +1, typical for pitch **[M]**. It uses signed levels at
  the ends (`SLv`, `RLv`). The port's model **[P][I]**: it starts at `SLv`, the attack takes it to
  0, it stays at 0 (`D` and `S` are unused) and the release goes to `RLv`; all of it scaled by
  `Levl`.
  - The default setup (`SLv` 0, `S` 1, `Levl` 1, with `Pitch Env → Pitch Coarse, Dept 12`) is in
    352 presets and has to be neutral. With "attack to 1 and sustain `S`" they all played an
    octave up.
  - The presets that use it agree with this model: FM Electro Bass (`SLv` 1, 40 ms attack, a +12
    pitch hit that falls to 0) and Reese Bass 2 (`SLv` −1, rises from −12).
  - `Levl` scales the output: Hoover Vacuum (`SLv` 0.74, `Levl` 0.195, Dept 100) gives a
    14-semitone drop; unscaled it would be 74.
  - Still to be confirmed in the binary (`0x10053163c`).

### Parameters

| Attr | Internal | Observed range | UI format **[B]** | Notes |
|---|---|---|---|---|
| `Dly` | Delay | 0..1 s | time | Wait before the attack |
| `A` | Attack | 0..10 s | `cFormatTimeAttack` | |
| `D` | Decay | 0..20 s | time | |
| `Brk` | BreakPoint | 0..1 | % | Level where `D` ends and `D2` starts **[I]** |
| `D2` | Decay2 | 0..2.5 s | time | Second decay, from `Brk` to `S` **[I]** |
| `S` | Sustain | 0..1 | % | |
| `R` | Release | 0..8.5 s or `INF` | `cFormatTimeInf` | `INF` = the note does not end on its own |
| `Levl` | Level | 0..1 | % | Output level (peak) |
| `SLv` | BiAttackLevel | -1..1 | signed % | Start level in Bipolar mode **[I]** |
| `RLv` | BiReleaseLevel | -1..1 | signed % | End level of the release in Bipolar **[I]** |
| `AC` | AttackCurve | 1.03..33.3 | see below | |
| `DC` | DecayCurve | 1.03..33.3 | | |
| `SC` | Decay2Curve **[I]** | almost always 1.22051 | | |
| `RC` | ReleaseCurve | 1.06..9.6 | | |
| `VLev` | VelLevel | -1..1 | signed % | How much velocity scales the level |
| `VTim` | VelTime | -1..1 | signed % | How much velocity scales the times |
| `KCtr` | KeyCentre | MIDI note (12..115) | integer | Note where key scaling is neutral |
| `KLev` | KeyLevel | -1..1 | signed % | Level scaling by note |
| `KTim` | KeyTime | -1..1 | signed % | Time scaling by note |

### Amplitude: the envelope squared **[P]**

The voice amplitude is `(envelope · velocity and key scaling · (Levl + Amp Gain))²`, measured
against NS2 renders:
- Sustain 0.4 sounds at −16 dB (0.4²), not −8.
- A 0.2 s attack is at −42 dB after 10 ms and at −11 dB after 100 ms.
- With `VLev` 1, velocity 40 sounds 20 dB below the maximum.
- `Levl` 0.5 drops 6 dB, so it is inside the square too.

The other envelopes are linear: the filter envelope routed to the cutoff matches NS2 without
squaring.

With Attack 0, the note starts at full level from the first sample **[P]**: a square wave jumps
to its peak. The port computes the amplitude per block and interpolates it, so on a free voice
the first block already starts at its final value, instead of rising from 0 over 64 samples.

### Velocity and key scaling **[B]**

Computed once at note-on (`0x1005314d4`, with note `n` and velocity `v ∈ 0..1`). `c` is `KCtr`:

```text
vel_level = VLev > 0 ? 1 + VLev·(v − 1) : 1 + VLev·v
key_level = n > c ? (KLev > 0 ? 1 − (n − c)·KLev/(128 − c) : 1)
                  : (KLev < 0 ? 1 + KLev·(1 − n/c)        : 1)
t_vel     = VTim·(64/127 − v)
t_key     = n > c ? (KTim > 0 ? (n − c)·KTim/c : 0)
                  : (KTim < 0 ? (n − c)·KTim/c : 0)
output   ×= vel_level · key_level
times    ×= 2^(3·(t_vel + t_key))
```

- With `VLev = 0.4`, a note with `v = 0.3` comes out at 0.72 of the level, not 0.3.
- The time factor probably applies to every stage **[I]**; the call is to `exp2f` **[I]**.

### Curves (`AC`, `DC`, `SC`, `RC`)

The XML stores the curve's **coefficient `k`** directly, between 1.03093 and 33.3333 **[P]**.
What the UI shows (`cEden2ADSR::cCurveShape`) is another scale **[B]**.

**Segment shape [B]**, decompiled from the function that draws the envelope (`0x1002ae5e0`):

```text
t ∈ [0, 1]              // elapsed fraction of the segment
y(t) = y0 + (y1 - y0) · t / (t + (k - 1)·(1 - t))
```

| k | Shape |
|---|---|
| 2 | Straight line |
| 1 < k < 2 | Convex: rises fast and flattens (RC-charge style). Default 1.22 |
| k → 1.03 | Almost a step at the start of the segment |
| k > 2 | Concave: starts slowly. At 33 almost all of the change happens at the end |

The audio DSP uses **the same formula** (`0x10053163c`, the envelope evaluator that the FM
connections also use) **[B]**.

**Times in the DSP [B]**: internally each time is a normalized value `x` in 0..1, with
`T = Tmax · x³` (a cubic curve). Attack/Decay/Release/Delay modulation is added in that `x`
domain and clamped to 0..1, and the segment advances `t += Δt / T` per sample.

| Segment | Observed `Tmax` |
|---|---|
| Attack | 10 s |
| Delay, Decay | 20 s |

If `T` is almost 0, a minimum of 1 ms is used. The XML stores seconds, so on load
`x = (T/Tmax)^(1/3)`. This explains the 10 s and 20 s maximums in the presets.

**UI ↔ coefficient conversion [B]** (`0x1002aeff8` and its inverse `0x1002af08c`). Only needed
to show or edit the value the way NS2 does:

```text
// UI u ∈ [0,1] → k
b = 2u - 1
if |b| ≥ 1e-6:
    c = 4/3
    d = (b ≥ 0) ? (2u - 2)(1 - c) + b : 2u(c - 1) - b
    b = b / d
u' = clamp(0.5·b + 0.5, 0, 1)
k  = 1 / (0.03 + 0.94·u')
```

Reference values: UI 0 → k 33.33, UI 0.25 → 6.78, UI 0.5 → 2 (straight), UI 0.707 → 1.22051
(the default), UI 0.75 → 1.173, UI 1 → 1.031.

### Modulation destinations of an envelope **[P][B]**

`Attack`, `Decay`, `Sustain`, `Release`, `Level`, `Delay`, `Retrigger`; the Pitch Env also has
`Attack Level` and `Release Level`. `Retrigger` restarts the envelope when the source crosses a
threshold **[I]**. The port restarts it when the input goes from ≤ 0 to > 0 while the key is
held; that way Arping On (LFO 3 → Env 4 Retrigger) plays its sixteenth-note rhythm as in NS2.
Some presets trigger it with Random LFOs of amplitude 0.01, so the threshold cannot be much
higher than 0.

---

## LFOs (`cLFOSimple`)

5 per voice (`LFO1..LFO5`) **[M][P]**.

### Shapes (`Type`)

| Type | Description **[M]** |
|---|---|
| `Sine` | Sine |
| `Square` | Square (`SqCv` rounds it **[I]**) |
| `Triangle` | Triangle |
| `Saw Up` / `Saw Down` | Saw |
| `Saw Double` | Double saw; the second one can be shifted for swing |
| `FM` | Free-running one-operator FM wave |
| `FM Sync` | Same as FM, but restarts on every LFO cycle |
| `Random` | Sample & hold, with steps or ramps |
| `Random Loop` | Random with a fixed sequence that repeats |

All of them are shaped by `Crv` (curve), `Warp` and `Qtz` (quantize) **[M]**.

### Parameters

| Attr | Internal | Range | Notes |
|---|---|---|---|
| `Rt` | Rate | 0..60 Hz | UI curve `Power<300>` **[B]** |
| `BRt` | RateUnits | `Hz` or a note denominator: `1` `2` `4` `8` `12` `16` `32` | With a value other than `Hz`, the period is `RtBt / BRt` of a bar **[I]** |
| `RtBt` | RateBeats | 1..16 | Numerator in tempo mode |
| `Sync` | Sync | `OFF` `KEY` `GLOBAL` | Restart per note or aligned to the song **[M]** |
| `OutT` | OutputType | `BIPOLAR` `UNI HALF` `UNI FULL` | `y`, `max(y, 0)` (half wave) or `0.5·y + 0.5` **[B]** |
| `Atk` | Attack | 0..25 s | **Linear** fade-in of the amplitude from note-on: `amp *= t/Atk` while `t < Atk` **[B]** |
| `Ampl` | Ampl | 0..1 | Output amplitude |
| `PhOf` | PhaseOffset | 0..1 | Start phase |
| `Crv` | Curve | 0..1 | On the sine it is a phase skew (see below) **[B]**; on the other shapes, the breakpoint **[I]** |
| `SqCv` | SqCurve | 0..1 | Square smoothing **[I]** |
| `Warp` | Warp | 0..1 | Rational curve on the output, the same as the envelopes' with `k = 1/(0.03 + 0.94·Warp)`. 0.5 = neutral **[B]** |
| `Qtz` | Quantize | 0..9 (0 = OFF) | `y = floor(y·n + 0.5) / n`; not applied to Random Loop **[B]** |
| `RPtn` | RandPattern | 1..16 | Seed or pattern of the Random Loop |
| `RLen` | RandLoopLen | 2..16 | Length in steps of the Random Loop |

### Decompiled details (`0x100560ccc`) **[B]**

- **Rate with modulation**: `rate = clamp(Rt + 60·m³, 0, 60)`, with `m` the Rate modulation. The
  cube gives fine resolution near 0.
- **Sine**: a 512-point table with linear interpolation (index = `|phase|·512/2π`).
- **Curve on the sine**: warps the phase. With `c` = Curve (+mod):
  `φ' = φ < c ? 0.5·φ/c : 0.5 + 0.5·(φ - c)/(1 - c)` and the output is `sin(2π·φ')`. With c = 0.5
  it is the plain sine. Below 0.02 and above 0.98 there are special cases (half sine).
- **Jump table by type**, in enum order: 0 Sine, 1 Square, 2 Triangle, 3 Saw Up, 4 Saw Down (the
  same routine as Saw Up, negated), 5 Saw Double, 6 FM, 7 FM Sync (shares the FM routine),
  8 Random, 9 Random Loop.
- **Random**: LCG `x = (x · 31415821 + 1) & 0x3FFFFFFF`, output `x·scale − 1` (bipolar). Random
  Loop uses a table of 16-step patterns (`RPtn` picks the pattern, `RLen` the length).
  Tempo: `RtBt 1`, `BRt 4` gives a period of one quarter note **[P]**: 480 ms in the NS2 project
  on the Mac, which runs at **125 BPM**. The port plays at 120 BPM because it has no transport.
  **One LFO period runs through the whole loop** **[P]**: each step lasts `period / RLen`.
  Arping On (8/16 of a bar, `RLen` 8) restarts its envelope on sixteenth notes in NS2. The
  pattern values are still **[?]**: the port uses the LCG with one seed per `RPtn`.
- **Warp**: `y' = y / (y + (k−1)(1−y))` for `y ≥ 0`, symmetric for `y < 0`, with
  `k = 1/clamp(0.03 + 0.94·Warp, 0.03, 0.97)`.
  Warp only applies to Sine, Triangle, Saw Up/Down and Saw Double; Square, FM and Random skip
  it.
- **Processing order**: waveform → output type (`OutT`) → warp → quantize → amplitude (`Ampl` +
  mod, clamped to 0..1) → `Atk` fade-in.

### Per voice or global?

The manual says "each voice has 5 LFOs" **[M]**. The LFO trigger (`0x100560bf0`) **[B]**:

- `Sync = GLOBAL`: the phase comes from the transport position.
- `Sync = KEY`: the phase starts at `min(PhOf, 0.999)`.
- `Sync = OFF`: it uses the phase it receives as an argument. If that is negative (a new note),
  it takes a **random** phase from the same LCG as the LFOs, × 0.999. Each note starts at a
  different point.

### Modulation destinations of an LFO **[P][B]**

`Rate`, `Level`, `Curve`, `Warp`, `Loop Pattern`.
