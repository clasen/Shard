# Obsidian (NanoStudio 2): a reverse-engineered specification

Documentation of NanoStudio 2's Obsidian synthesizer, meant as the basis for reimplementing it
on another platform (web with AudioWorklet/WASM, ESP32-S3, desktop). It describes *what* the
engine does and *how* its data is stored; it contains no code or assets from the app.

## Contents

| File | Content |
|---|---|
| [01-architecture.md](01-architecture.md) | Signal flow, voice, modules and internal class map |
| [02-preset-format.md](02-preset-format.md) | The full `.obs` (XML) format, attribute by attribute |
| [03-oscillators.md](03-oscillators.md) | The 7 oscillator types and their parameters |
| [04-filters.md](04-filters.md) | Voice filters, routing and the global filter |
| [05-envelopes-lfos.md](05-envelopes-lfos.md) | 5 envelopes and 5 LFOs |
| [06-modulation.md](06-modulation.md) | Modulation matrix: sources, destinations, parameters |
| [07-effects.md](07-effects.md) | Multi-FX, Delay, Reverb and their order |
| [08-wavetable-format.md](08-wavetable-format.md) | The binary `.tbl` format |
| [09-open-questions.md](09-open-questions.md) | What is still unknown and how to find out |
| [10-porting-notes.md](10-porting-notes.md) | Notes for web and ESP32 |

## Sources

- **Factory presets**: 312 files `Factory/Instruments/Obsidian/*.obs/Package.obs` inside
  `NanoStudio 2.app/Wrapper/NanoStudio 2.app/`. The schema and the ranges were extracted by
  walking all of them.
- **Manual**: `Manual/Obsidian.html` (v2.1.2) inside the bundle.
- **Binary**: `NanoStudio 2` (Mach-O arm64, SHA-256 `310ed846…5a13`). It is stripped, but it
  keeps the C++ RTTI names and the UI's string tables, which give the classes, the parameter
  names, the formats and the UI curves.
- **Wavetables**: `WaveTables/**/*.tbl`, analysed byte by byte.

## Confidence levels

Every claim carries a mark:

- **[P]** observed in the presets (a concrete, verifiable fact).
- **[M]** stated in the manual.
- **[B]** observed in the binary (RTTI, strings), without running or decompiling.
- **[I]** a reasonable inference, not verified.
- **[?]** unknown; see [09-open-questions.md](09-open-questions.md).

## Status

- Done: architecture, the full preset format, all enums, the XML attribute → internal parameter
  map, the `.tbl` format (structure and samples).
- Decompiled: the cutoff and keytrack formula, the filter types' internal codes, the shape of
  the envelope curves and the UI conversion, LFO details (rate, curve, warp, random, output
  types, quantize).
- Also decompiled: Q and the multimode ladder of the `-A` filters, how modulation is added in
  the filter, oscillator pitch, Analog (wavetable with mipmaps), NanoSaw (7 DPW saws), PD
  (CZ style) and the wavetable detune modes.
- Also: FM (fixed point, band-limited waves), Noise (decimation that follows the note), reverb
  (Freeverb + Dattorro plate) and Delay.
- Also: the ladder's per-sample process, the DSP envelopes (`T = Tmax·x³`), Formant (Peterson &
  Barney), waveshapers, Multi-FX (types → engines) and which reverb algorithm each room uses.
- Measured on hardware: per-block cost on a real ESP32-S3; see
  [10-porting-notes.md](10-porting-notes.md).
- Pending: the `Dept` scale in the matrix (there is a render test in
  [06](06-modulation.md#scales)), the exact waveshaper curves and the Multi-FX's internal
  ranges. See [09-open-questions.md](09-open-questions.md).

## Legal note

Reading the preset format and reimplementing the architecture for your own use is
interoperability. The factory presets, wavetables and samples are Blip Interactive's content:
do not redistribute them inside another product.
