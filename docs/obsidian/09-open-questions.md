# Open questions

Sorted by impact on the sound. For each one: what is missing and how to find out.

Methods:
- **D** = decompile the binary (Hopper through REA). The UI formats the normalized value for
  display, so each parameter's `Format` function usually contains the formula. Example:
  `Eden2::cUIParamFormatCutoffFreq`.
- **R** = test render in NS2: a minimal patch, record the output and measure.
- **X** = compare presets or data files.

| # | Question | Method | Where to look |
|---|---|---|---|
| 1 | ~~`CutO` 0..1 → Hz~~ **Solved**: `20·900^v`, keytrack relative to C3 ([04](04-filters.md#cutoff-formula-b)) | D | `0x1000a2c94`, `0x10055a290` |
| 2 | ~~`Q` 0..1 → resonance~~ **Solved**: formulas per family and multimode ladder ([04](04-filters.md#q-and-resonance-b)). The ladder's per-sample process and `Drive` (cubic saturator) are solved too | D | `0x1005399d8`, `0x10054b098`, `0x100556498` |
| 3 | Topology: `-A` ladder, Formant (Peterson & Barney), WS (exact Sine/Tri curves, the rest approximate) and Lo-Fi **solved**. The formants' Q is missing | D | `0x100549a90` |
| 4 | ~~Shape of the envelope segments~~ **Solved** (UI and DSP), and the times `T = Tmax·x³` ([05](05-envelopes-lfos.md)) | D | `0x10053163c` |
| 5 | Scale of `Dept`. **Partial**: `m = 1` = the parameter's maximum (filter). The `Dept → m` conversion does not show up as an `scvtf` of field `+0x20` in the static code; left for the render test | R | Render test in [06](06-modulation.md#scales) |
| 6 | ~~How modulation is added~~ **Solved** for the filter: in the parameter's units, clamped to its range | D | `0x10055a290` |
| 7 | ~~Analog antialiasing~~ **Solved**: Sine/Tri/Saw/Square are `Analog.tbl` with mipmaps ([03](03-oscillators.md)). The Pulse/Bi-Pulse details are missing | D | `0x100562188`, `0x1004e2234` |
| 8 | ~~`SRat` → ratio~~ **Solved**: `2^(4·SRat)`. Still to confirm which mode is `ANALOG` and what the hardness does | D | `0x100562188` |
| 9 | ~~NanoSaw~~ **Solved**: 7 DPW saws, detunes and gains ([03](03-oscillators.md)) | D | `0x10046d1c4` |
| 10 | PD formulas. **Partial**: structure, `PD` curves and the pitch limit. The exact breakpoints of Square/Pulse/Double Sine/Saw Pulse are missing | D | `0x10056da2c` and routines |
| 11 | Mip selection and crossfade in wavetable. Detune/Spread modes **solved** ([03](03-oscillators.md)) | D | `0x10058cb60` |
| 12 | The `unknown_a`, `pairs` and `gain` fields of the `.tbl` | D | `cWaveTable` loader |
| 25 | ~~FM~~ **Solved**: render, feedback and the `Amt → integer` factor (`K·a²`) | D | `0x100565478` |
| 26 | Noise: `Cut`/`Q` **solved**. Still to confirm whether types 3..6 filter white or brown noise | D | `0x10056cd20` |
| 13 | ~~Unison~~ **Solved**: detune, pan and level tables by voice count ([02](02-preset-format.md#unison-b)) | D | `cUnisonVoice`, `0x100559afc` |
| 14 | Glide: exponential in pitch? Constant time or per interval? | D, R | `cSynthVoice` |
| 15 | Exact `FMix` and `Ring` | D | `cSynthOsc` |
| 16 | `STEREO` routing: how are the mono oscillators split? | D | `cFilters` |
| 17 | ~~Reverb algorithms~~ **Solved**: Freeverb (16 combs) + Dattorro plate ([07](07-effects.md#topology-b)). Which `Algo` uses each engine is missing | D | `0x1001907cc`, `0x1002b0ab8` |
| 18 | Multi-FX: type mapping **solved** ([07](07-effects.md)). The internal delay/LFO ranges per mode are missing | D | `0x1002d1a20`, `0x100249b18` |
| 19 | ~~Layers `LL1..LX2`~~ **Solved** ([03](03-oscillators.md#velocity-layers-and-splits-osc--cosctriple-b)). Still to confirm that `v = vel/127` and the UI's `VEL` | D | `0x1005a6178` |
| 20 | Exact meaning of `FX/@Ord` | X, D | `cEden2Effects` |
| 21 | `Tags` bits: categories inferred by name ([02](02-preset-format.md)); still to confirm in the UI, and the remaining bits | X | Compare presets with tags known in the UI |
| 22 | ~~`Autm`~~ **Solved**: C02..C11 = Pad X, Pad Y, Knob 1..8 ([02](02-preset-format.md#autm)). `C01` is missing | X | — |
| 23 | `Poly=0` | R | Patch with Poly=0: count the voices |
| 24 | Units of `Dtn` (osc): **semitones** (solved). Zone `Tune`: the render uses `Root − Tune`; still to confirm that they are semitones | D, R | `0x10057020c` |

## Method notes

- Hopper needs about 2'05" to analyse the binary. With `REA_HOPPER_STARTUP_TIMEOUT_MS=600000` in
  the `env` of the `rea` MCP server, REA waits for it without trouble.
- The binary uses chained fixups: typeinfo and vtable pointers have no xrefs in Hopper. What
  works is going in through **constants** (scanning the binary for `movz`/`movk` pairs with the
  float you are looking for) or through **format strings** (`"%0.01f Hz"`) and following their
  xrefs from the code.
- Constants computed at startup (such as `K = log2(900)`) are 0 in the file; you have to find
  the static initializer that writes them (xrefs to the global).
