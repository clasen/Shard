# Architecture

## Summary

- Polyphonic synthesizer with up to **32 voices** **[M]**, and unison of 1 to 4 voices per
  note **[P]**.
- Each voice has **3 oscillators**, and each can use one of **7 synthesis methods**:
  Analog, Wavetable, PD, FM, NanoSaw, Noise and Sample **[M][P]**.
- Each voice has **2 filters**, in series, in parallel or as a stereo pair **[M][P]**.
- There are **5 envelopes** and **5 LFOs** per voice **[M]**. Only the amplitude envelope has a
  fixed connection.
- The **modulation matrix** has 24 slots **[P]**, with an optional macro multiplier.
- There are **10 macros**: 8 knobs and an X/Y pad **[M]**.
- The voice mix goes through a **stereo global filter** and then through the
  **Multi-FX → Delay → Reverb** chain, whose order can be changed **[M]**.

## Signal flow

```
                 ┌───────────────────── VOICE (×N, + unison) ─────────────────────┐
 note/vel ──►    │  Osc1 ─┐                                                       │
                 │  Osc2 ─┼─► [FilterMix per osc] ─► F1 ─┐                        │
                 │  Osc3 ─┘                        ─► F2 ─┴─► (routing) ─► VCA ───┼──┐
                 │   ▲ ring mod Osc(n-1)×Osc(n) [I]                       ▲       │  │
                 │   │                                              EnvA (fixed)  │  │
                 │  Osc1 can bypass the filters / VCA (O1BP)                      │  │
                 └────────────────────────────────────────────────────────────────┘  │
                                                                                     ▼
                                   Σ voices ─► Global filter (F3, stereo) ─► FX chain ─► out
                                                              MFX ─ Dly ─ Rev (order = Ord)
```

### Filter routing (`Rout`) **[M][P]**

Decompiled from the voice (`0x100403f68`, gains at `0x1004036c0`) **[B]**:

- Each oscillator is split between two **mono buses** by its `FMix`: F1 bus =
  `osc·(1 − FMix)`, F2 bus = `osc·FMix` (`0x1003af0cc`). The split does not depend on `Rout`.
- Each active filter processes its bus. A disabled filter passes its bus through. The output
  gain (`OGn`) is applied when mixing.
- The `Rout` enum is 0 `PARALLEL`, 1 `SERIES`, 2 `STEREO`.
  - `PARALLEL`: each filter's output goes to the stereo output with its `Pan` (constant-power
    law, sine/cosine).
  - `SERIES`: F1's output is added to F2's bus (mono), and F2 goes to the output with its `Pan`.
  - `STEREO`: **F2 uses F1's parameters** (F1's patch is passed when computing F2), and both
    outputs are panned the same way, with ×2 gain. The manual says "F1 left, F2 right" **[M]**.
    Whether the oscillators' `Pan` splits between the two buses in this mode is **[?]**.

`O1BP` (`Osc1Bypass`) = `FILTER` makes oscillator 1 skip the voice filters. The manual also
mentions bypassing the amplitude envelope; the presets only show the values `OFF` and
`FILTER` **[M][P]**.

## Internal class map **[B]**

Real names from the binary, useful as a guide for naming modules in a reimplementation.

| Internal class | Role |
|---|---|
| `Eden2::cEden2` | The Obsidian instrument (patch: `Octave`) |
| `Eden2::cSynthVoice` | Voice management: polyphony, unison, glide |
| `Eden2::cOscTriple` | The 3 oscillators with velocity layers/splits |
| `Audio::cSynthOsc` | One oscillator: common parameters + type |
| `Audio::cOscTypeAnalogue` | Analog oscillator |
| `Audio::cOscTypeWaveTable`, `Audio::cWaveTable` | Wavetable and its loaded table |
| `Audio::cOscTypePD` | Phase distortion |
| `Audio::cOscTypeFM` (`cOperator`, `cConnection`) | 3-operator FM |
| `Audio::cOscTypeNanoSaw` | Supersaw |
| `Audio::cOscTypeNoise` | Filtered noise |
| `Eden2::cFilters` | Filter routing |
| `Audio::cSynthFilter` | One multimode filter (includes waveshapers, comb, formant) |
| `Audio::cEnvelopeADSR` | Envelope (ADSR / AD / Bipolar) |
| `Audio::cLFOSimple` | LFO |
| `Eden2::cEden2Effects` | Effects chain (enable + order) |
| `Audio::cEffectMultiFXSimple` | Chorus/Ensemble/Flanger/Phaser |
| `Audio::cEffectDelaySimple` | Stereo/cross/pingpong delay |
| `Audio::cEffectReverbAlgoSimple` | Algorithmic reverb ("simple" version of Algoverb) |

Obsidian's effects are reduced versions (`*Simple`) of NanoStudio's full insert effects:
`cEffectMultiFX`, `cEffectDelay`, `cEffectReverbAlgo` **[M][B]**. The full versions have more
parameters, listed in [07-effects.md](07-effects.md).

## UI concepts that matter for the engine

- **Rapid connection**: UI only; it ends up creating a slot in the modulation matrix **[M]**.
- **Macros**: the knobs and the pad are modulation sources (`Knob 1..8`, `Pad X`, `Pad Y`).
  They can also be multipliers (`Sca`) of another connection **[M][P]**.
- **Recently Modified / Default**: patch management; it does not affect the sound.
