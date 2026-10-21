# Notes for a port

My own design decisions, not observations of NS2. They separate the *what* (the other
documents) from the *how* on each platform.

## Recommended structure

A DSP core in **C++ with no platform dependencies**, with two frontends:

```
core/            C++17, no heap on the audio thread, processes blocks of N samples
  preset/        .obs XML parser → struct Patch (off the audio thread)
  dsp/           osc, filter, env, lfo, modmatrix, fx
  voice/         voice allocation, unison, glide
web/             core compiled to WASM + AudioWorkletProcessor
esp32/           core + I2S + USB-MIDI (ESP-IDF)
tools/           converters (.obs → compact binary, .tbl → C array)
```

That way a preset sounds the same in the browser and on the hardware, and the web build is
useful for iterating fast and comparing against NS2 renders.

## Priorities (by use in the factory presets)

1. A wavetable engine with mipmaps (it covers Analog Sine/Tri/Saw/Square and Wavetable at once,
   see [03](03-oscillators.md)), LP 12-A / LP 24-A filters, EnvA/EnvF/EnvP, sine LFO,
   modulation matrix, Chorus 1, Stereo Delay, Small Room reverb.
2. Wavetable Detune/Spread modes, Pulse, NanoSaw, Noise White, unison, glide.
3. FM, PD, the remaining filters, the remaining FX.
4. Sample and spectral loop.

## Web

- `AudioWorkletProcessor` with 128-sample blocks; the core in WASM with `SharedArrayBuffer` or
  `postMessage` for the parameters.
- The `.obs` parser can stay in JS (DOMParser) and pass a flat struct to the worklet.
- There is no real CPU limit for 32 voices with everything on.

## ESP32-S3 N16R8

- Blocks of 32 to 64 samples, audio on one core and MIDI/UI on the other.
- Use `float` throughout the DSP (the S3 has a single-precision FPU); no `double`.
- Wavetables as int16 in flash, with the active mips copied to internal SRAM.
- Delay and reverb lines in PSRAM (sequential access).
- Parse the presets on the PC and store them in a compact binary format: no XML parser needed
  on the microcontroller.
- Measure cycles per voice (`esp_cpu_get_cycle_count`) from the first prototype to settle the
  real polyphony.

### Hardware measurements (ESP32-S3 N16R8, 240 MHz, OPI PSRAM)

Benchmark in [`bench/obsidian_bench`](../../bench/obsidian_bench/obsidian_bench.ino): 64-sample
blocks, 48 kHz, Arduino-ESP32 3.3.8 (`-Os`), float. The budget of one core is
240 MHz / 48 kHz = **5000 cycles per sample**.

| Block | Cycles/sample | % of one core |
|---|---|---|
| Wavetable oscillator (int16, linear interp., 2-frame crossfade) | 67.9 | 1.4 % |
| DPW saw (1 of the NanoSaw's 7) | 54.8 | 1.1 % |
| NanoSaw (7 DPW saws) | 435.6 | 8.7 % |
| 4-pole ladder + clip with `fminf`/`fmaxf` | 205.6 | 4.1 % |
| 4-pole ladder + clip with comparisons | **77.9** | 1.6 % |
| Biquad (`-D` filters, EQ) | 16.8 | 0.3 % |
| Rational envelope per sample (1 division) | 102.1 | 2.0 % |
| Sine LFO (512-entry table) | 56.8 | 1.1 % |
| Voice: 3 WT osc + 2 ladders + 2 envelopes, naive | 704.7 | 14.1 % |
| Optimized voice (envelopes per block, clip with comparisons) | **363.6** | 7.3 % |
| Freeverb 16 combs, buffers in internal SRAM | 1153.7 | 23.1 % |
| Freeverb 16 combs, buffers in PSRAM | 1274.8 | 25.5 % |
| Dattorro plate, internal SRAM | 700.4 | 14.0 % |
| Dattorro plate, PSRAM | 774.9 | 15.5 % |

**Estimated polyphony** with the optimized voice: ~13.8 voices per core without FX; ~11.6
sharing the core with the plate; **~17 voices** using both cores at 70 % (the rest is left for
MIDI, the modulation matrix, I2S and headroom), with the plate in PSRAM.

**Lessons:**

- On Xtensa, `fminf`/`fmaxf` are **library calls**: replacing them with comparisons took the
  ladder from 206 to 78 cycles.
- The per-sample division of the envelope curve is expensive. Evaluating it at control rate
  (once per block) and interpolating linearly halves the voice, and NS2 does something
  equivalent with its parameters (slews and per-block coefficient interpolation).
- Octal PSRAM costs only ~10 % more than internal SRAM for sequential-access reverbs. It pays
  off: the internal SRAM (~300 KB free) is kept for wavetables and voice state.
- The Dattorro plate performs better than Freeverb (775 versus 1275 cycles) and covers Dense
  Room, Plate and Hall. Freeverb is only needed for Small/Large Room.
- NanoSaw is the most expensive oscillator (7 saws). With NanoSaw on all 3 oscillators the voice
  grows by about 1100 cycles: better to limit it to one NanoSaw per voice or use fewer saws.
- Not measured yet: FM (3 int16 operators), PD, Noise, Multi-FX and Delay. Given their
  structure, each should cost on the order of one wavetable oscillator.

The board was left with the benchmark firmware. The backup of the previous firmware is at
`bench/backup/esp32s3_boot_nvs_apps_0x0-0x410000.bin` (bootloader, NVS and both apps; the FAT
partition was not touched). To restore it:

```bash
esptool --port /dev/cu.usbmodem214201 write_flash 0x0 bench/backup/esp32s3_boot_nvs_apps_0x0-0x410000.bin
```

### First firmware that makes sound

[`firmware/obsidian_esp32`](../../firmware/obsidian_esp32/) (Arduino-ESP32 3.3.8, same board
options as the benchmark):

- **Output**: PCM5102A over I2S, stereo, 16 bits, 48 kHz. Pins **BCK = GPIO17, LCK = GPIO16,
  DIN = GPIO15**.
- **Engine** (`synth.h`):
  - 8 voices.
  - Each voice: 3 wavetable oscillators (Sine/Tri/Saw/Square like Obsidian's Analog) with
    per-octave mipmaps generated on the board by additive synthesis (145 ms at startup, without
    NS2's tables), an LP24 ladder, cutoff `20·900^x` with keytrack relative to C3, and amplitude
    and filter envelopes with the rational curve at block rate.
  - Glide in semitones.
  - Dattorro plate in PSRAM and soft saturation at the output.
- **Audio on core 1**, a top-priority task. Arduino's `loop()` (serial console) runs on the same
  core at lower priority; core 0 is left free (Wi-Fi/BT off).
- **Measured load**: ~77–81 % of core 1 with the demo (up to 8 voices + plate).
- **Use**: at startup it plays an Am–F–C–G progression with an arpeggio. Over the serial console
  (115200) it accepts `on <note> [vel]`, `off <note>`, `demo 0|1`, `cut`, `res`, `env`,
  `wave <osc> <0..3>`, `rev`, `vol`, `glide`, `stat` and `help`.

**Update: reverb on core 0 and MIDI** (`obsidian_esp32.ino`, `usb_midi_host.h`):

- The plate runs in its own task on **core 0**. Core 1 renders the voices of block *n* while
  core 0 processes the reverb of block *n−1* (one block, 1.3 ms, of latency on the wet signal
  only). Measured with 5–6 voices: **core 1 ~33–40 %**, core 0 ~30–35 %. That is why polyphony
  went up to **10 voices**.
- **MIDI**: notes, pitch bend ±2 semitones, sustain (CC64), all notes off (CC123) and CCs
  assignable with **MIDI learn** (`learn <param>` over serial; the map is saved in NVS). By
  default: CC74 cutoff, CC71 resonance, CC1 filter envelope amount, CC91 reverb, CC7 volume,
  CC5 glide, CC73 attack, CC75 decay, CC79 sustain, CC72 release. Assignable parameters:
  `cut res env rev vol glide atk dec sus rel fdec det lforate lfoamt`. `lforate` and `lfoamt`
  move the rate (0.05–20 Hz) and the amplitude of LFO 2, which in Obsidian's layout goes to the
  cutoff (436 presets): the filter wobble.
- The demo turns itself off with the first MIDI note.
- The **USB Mode** board option picks the MIDI transport:

  | USB Mode | MIDI | Serial console | Upload |
  |---|---|---|---|
  | `USB-OTG (TinyUSB)` | the board is a USB-MIDI **device** for the Mac (`ESP32S3_DEV`) | same native port (CDC) | `UploadMode=cdc` |
  | `Hardware CDC and JTAG` | the board is a **host**: a class-compliant controller on the native port | COM port (UART0) | through the COM port |

- [`firmware/tools/midi_bridge.py`](../../firmware/tools/midi_bridge.py) (macOS, no
  dependencies, uses CoreMIDI through `ctypes`): `list`, `test <dest>`,
  `bridge <source> <dest>` and `monitor <source>` to see a controller's CCs.

**Korg nanoKEY Studio** (factory scene, measured with `midi_bridge.py monitor nanokey` over USB
and with `mon 1` over Bluetooth). The Korg uses a different layout on each connection:

| Control | USB | Bluetooth | In the firmware |
|---|---|---|---|
| Keyboard (with OCT −/+) | notes, channel 1 | notes, channel 1 | notes |
| X/Y pad | pitch bend + CC1, channel 1 | same | bend ±2 st and filter envelope amount |
| Knobs 1–8 | CC16–23, channel 16 | CC20–27, channel 1 | cut, res, atk, rel, lforate, lfoamt, rev, vol |
| Pads 1–8 | notes 64–71, channel 2, with velocity | CC95–102, channel 1 (127 on press, 0 on release) | presets 0–7 |

The knobs' CCs overlap between the two layouts (CC20–23), so the firmware recognizes them by
channel (`kKorgLayouts`), not by the CC map. A CC assigned with `learn` takes priority.

**Presets**: 8 patches in the firmware (Poly, Pad, Bass, Lead, Pluck, Organ, Brass and Abandoned
Factory). The first seven are my own; **Abandoned Factory** (pad 8) is the factory preset
`Factory/Instruments/Obsidian/Abandoned Factory.obs` translated by hand. They are picked with the
pads, with Program Change (`program % 8`) or with `preset <0..7>` over serial. The pads'
messages do not play notes; everything else is processed in omni. `mon 1` prints every incoming
MIDI message (USB or Bluetooth) over serial.

**Bluetooth MIDI** ([`ble_midi_central.h`](../../firmware/obsidian_esp32/ble_midi_central.h)):
in both USB modes the board acts as a BLE central (the core's `BLE` library, Bluedroid).

- It scans, connects without pairing to the first peripheral that advertises the BLE-MIDI
  service (`03B80E5A-…`) and subscribes to the I/O characteristic (`7772E5DB-…`).
- It decodes the packets (header, timestamps, running status, SysEx discarded) into the same
  `handleMidi` as USB.
- If the link drops, it scans again and reconnects on its own.
- Tested with the nanoKEY Studio on batteries.
- The nanoKEY only sends notifications over an encrypted link: the central pairs (Just Works,
  with bonding and no PIN) before subscribing. Without that it connects but no message arrives.
- The peripheral accepts a single connection: if it was connected to the Mac, it has to be
  disconnected there.

**Modules added to `synth.h` to translate factory presets**:

- **Noise** oscillator (`kOscNoise`):
  - The integer generator, the note-following sample & hold (`Deci`) and the biquad of the
    Low/High/Band Passed and Notched types, with the `f` and `q` formulas from
    [03](03-oscillators.md#noise-nois--cosctypenoise).
  - Pink and Brown are missing.
- **Imported wavetable** oscillator (`kOscTable`): frames from an NS2 `.tbl` with a linear
  crossfade driven by `Pos` (+ modulation).
- **Ring** per oscillator: a mix towards `osc(n−1) × osc(n)`. An oscillator at level 0 keeps
  running if the next one uses it for the ring.
- **LP 12-D** filter (biquad, `Q = 0.5 + 5.5·q²`) besides the LP 24-A ladder, and `OGn`.
- **Envelopes**:
  - Per-segment curves (`AC`/`DC`/`RC`) and `Levl`.
  - Decompiled velocity and key scaling (`VLev`, `VTim`, `KCtr`, `KLev`, `KTim`).
  - My own presets use `VLev = 1` on the amplitude envelope.
- **Per-sample ramps**: the ladder and biquad coefficients are computed per block and linearly
  interpolated sample by sample, as in NS2. The wavetable position is also interpolated within
  the block. Without this, fast LFOs on the cutoff produce audible steps.
- **General modulation matrix** ([06](06-modulation.md)): 24 slots
  `source × multiplier × Dept/100` summed per destination, evaluated once per block.
  - **Sources**: Const, Rand 1/2 (0..1 per note **[I]**), Note (`note/127` **[I]**), Velocity,
    Gate, the 5 envelopes, the 5 LFOs, Pitch Wheel (and its halves), Knob 1..8, Pad X/Y.
  - **Destinations**:
    - Per oscillator (`Osc All` adds to all three): Pitch Coarse, Pitch Fine, Level, Ring Mod,
      WT Position and Noise Cutoff/Q/Decimate.
    - Filter 1: Cutoff, Cutoff Slow (50 ms one-pole **[?]**), Q and Output Gain (`2·m`).
    - Amp Gain: `×clamp(1 + m, 0, 2)` **[?]**.
    - Envelopes: A/D/R in the `x` domain with `Tmax` 10/20/20 s; S and Level are added.
    - LFOs: Rate (`Rt + 60·m³`) and Level.
    - Engine-level: cutoff and Q of the global filter, Reverb Mix and Multi-FX Mix.
  - The envelopes and LFOs use the previous block's matrix for their own parameters. Those that
    no slot reads are not computed.
  - Engine-level destinations have no owning voice. They take the per-voice sources from the
    newest voice, and the LFOs from 5 free-running engine LFOs **[?]**.
  - `Pitch Coarse` is in semitones with `m = 1` = 100 semitones **[I]**: 304 factory presets
    bend with `Pitch Wheel → Pitch Coarse, Dept 2` (±2 semitones), and the hats rise an octave
    with `Dept 12`. `Pitch Fine` uses ±1 semitone **[I]**.
  - When a preset loads, the knobs and the pad take the values from `Autm` `C02..C11`. The
    Korg's knobs control the same things in every preset (cut, res, atk, rel, fdec, det, rev,
    vol) and do not move the macros. They used to move the preset's macro if it used one. That
    way, in the Rhodes knob 1 was Amp Attack and not the filter.
  - The volume knob has an audio taper (`volume = x²`). With the limiter linear up to 0.5, a
    linear scale hit the ceiling in the first third of the travel.
  - My own presets (0..6) start with a `Pitch Wheel → Osc All Pitch Coarse` slot of ±2
    semitones. Bend is no longer a special case in the engine.
- **Full LFOs** (`cLFOSimple`, [05](05-envelopes-lfos.md#lfos-clfosimple)):
  - Shapes: Sine (with `Crv`, over the sine table), Square, Triangle, Saw Up/Down, Saw Double
    and Random (decompiled LCG).
  - Random Loop repeats `RLen` steps from one seed per `RPtn`; NS2's patterns are **[?]**.
  - `OutT`, `Warp`, `Qtz`, `Atk` (fade-in) and `PhOf`.
  - Sync: OFF (random phase), KEY (`PhOf`) and GLOBAL (the engine LFO's phase stands in for the
    transport).
  - Tempo-synced rates play at 120 BPM. FM and FM Sync are missing.
- **Global comb filter** (F3 `Comb Pos`/`Comb Neg`), stereo:
  - Decompiled algorithm ([04](04-filters.md#formant-waveshapers-and-lo-fi-b)).
  - Keytrack on the last note.
  - The delay is interpolated per block, without NS2's crossfade between reads.
- **Stereo voices and unison** (`Uni`, `UDtn`, `USte`): detune, pan and level with the decompiled
  tables ([02](02-preset-format.md#unison-b)).
  - Linear pan law with gain 1 at the centre **[?]**.
- Stereo **Phaser** in the Multi-FX stage:
  - An allpass chain with feedback and an LFO offset by `Wid`.
  - Rate `0.02 + 4·Rat²` Hz, sweep from 150 Hz to `150·2^(8·Dep)` Hz, feedback `0.9·FB`. NS2's
    scales are **[?]**.
- **Mono** mode (`Poly=1`) with legato glide.
- **Sample** oscillator (`kOscSample`), see [below](#content-image-presets-wavetables-and-samples-in-flash):
  - One zone per note (the first whose `Lo..Hi` contains the key), without NS2's crossfade
    between overlapping zones or its second zone.
  - Read at `2^((pitch + Tune − Root)/12)·sr_file/48000`, linear interpolation and forward loop
    of the `smpl` chunk; linear `Vol`. No `Loop=Spectral`, `Fwd-Rev`, zone `Pan` or the
    `0.7071` of the centre pan.
- **AD and Bipolar envelopes**, plus `EnvP`, `Env4` and `Env5` per voice:
  - AD goes down to 0 after the decay and ignores the note-off. Bipolar starts at `SLv` and goes
    to `RLv` on release. Both are readings of the manual **[I]**: the state machine was not
    decompiled.
  - An oscillator's `Level` follows `clamp(Lev + 2·m, 0, 2)` and is interpolated within the
    block.
- Per-oscillator `KeyT` (`60 + (note − 60)·KeyT`). Unison detune and pitch modulation are not
  scaled.
- **FM** oscillator (`kOscFM`), as in [03](03-oscillators.md#implementation-b-0x100565478):
  - 32-bit phases and the 15 band-limited shapes extracted from the binary with
    [`fmwaves2h.py`](../../firmware/tools/fmwaves2h.py) (`fm_waves.h`, 43 KB, local use).
  - Averaged feedback and modulators with one sample of delay.
  - Amounts `(1 − Levl + Levl·env) × velocity × (Amt + mod)` squared times `K`, interpolated
    within the block. The 8 connection envelopes run per voice.
  - Matrix destinations: `FM OPx->OPy`, `FB`, `Out` and `Freq` (`ratio + 10·m`).
  - As in NS2, the render is specialized: an operator only runs if it reaches the output in that
    block.
- **Filter slots** (`Filter`) for F1, F2 and the global filter F3, with LP 24-A, LP 12-A,
  HP 12-A, HP 24-A, BP 6-A, BP 12-A, Notch 6-A and Notch 12-A on the ladder, and LP 12-D,
  HP 12-D, EQ Peak and EQ Shelf on the biquad.
  - A non-comb F3 runs in stereo after the voice mix. It follows the last note's keytrack and
    takes cutoff and Q from the matrix.
  - The global ladder receives half the mix, like F1, and the output is compensated ×2.
- **F2 and filter routing** ([01](01-architecture.md#filter-routing-rout-mp)):
  - Second filter per voice, per-oscillator `FMix` and `O1BP`. `PARALLEL` and `SERIES` as in
    NS2, and F1 and F2 output `Pan` with the port's linear law.
  - `STEREO` runs both buses through F1, because F2 runs F1's configuration.
  - Matrix routes to `Filter 2` (Cutoff, Q, Output Gain); `Filter 1+2` reaches both.
  - Per-oscillator `Pan` is missing.
- **Waveshapers** (`WS Tube/Soft/Hard/Enhance/Sine/Tri`) as an F1, F2 or F3 type, with the
  decompiled curves in 1025-point tables (in the `WaveBank`, PSRAM) and the `Drv` and `WSGn`
  gains ([04](04-filters.md#formant-waveshapers-and-lo-fi-b)). No oversampling: the folders
  (Sine, Tri) alias.
- **NanoSaw** (`kOscNanoSaw`): the 7 DPW saws with the decompiled detune curve, spread and gains
  ([03](03-oscillators.md#nanosaw-ns--cosctypenanosaw)), in mono. Below 40 Hz the saw is read
  directly (DPW divides by the frequency).
- **PD** (`kOscPD`): the saw as in NS2 (the amount curve, with the knee at 0.25 and
  `c = −0.3067` so it is continuous). The other eight shapes are readings of the Casio CZ with
  that curve and the documented exponents; the resonant ones window a sine at `r = 1 + 16·PD²`
  times the pitch **[I]**. The steep section lasts at least 4 samples, instead of NS2's
  per-pitch polynomial **[I]**.
- The biquad does not go below 20 Hz: lower down, float rounding and the coefficient ramp made
  it diverge (Slow Sweep Pad, with a random LFO on an LP 12-D at ~30 Hz).
- **Amp Gain** is added to the amplitude envelope's `Levl` **[I]**. Arping On has `Levl` 0 and
  opens the amplitude with Env 4 → Amp Gain.
- **Delay** (`Dly`): Stereo, Cross and Pingpong 1:1..1:4, see
  [07](07-effects.md#delay-dly--ceffectdelaysimple).
- **Decompiled ladder, with Drive** (2026-10-20): the `Ladder` follows NS2's setup and render
  ([04](04-filters.md#q-and-resonance-b)). That includes the 1-pole stages with a zero
  (`0.769/0.231`), polynomial `G` and `k`, the cubic saturator with `sat0`/`sat1` set by `Drv`,
  the per-mode Q compensations and the ten modes of the table.
  - `Drv` of F1, F2 and F3, and the matrix destinations `Filter 1/2 Drive`.
  - It replaces the approximate ladder (`tanf`, `k = 3.6·q`, hard clip at ±1.2), which clipped
    loud mixes. With 3 notes on the host, the median RMS of the 448 presets went up ×1.7
    (+4.6 dB). The output still goes through `mixOut`'s soft limiter, and on the board the peaks
    sit between −1 and −7 dBFS.
  - Electric Bubbles and Doppler's Bike, which were silent, now play.
  - The limit `k ≤ 3/(sat0·sat1)` is active **[I]**: without it, the basses with Drive and some
    resonance self-oscillated and sounded much worse.
  - `kOutputTrim` (−3 dB) lowers the output before the soft limiter to make up for the higher
    level. The value was picked by ear on the board.
  - The output limiter was `x/(1 + |x|)`, which compresses and distorts from low levels (at 0.2
    it already takes off 17 %). Now it is linear up to `kClipKnee` (0.5) and curves gently
    towards full scale above that.
- At startup, `kBootPreset` (RhodesMK2Raw) is loaded if the content image has it.
- **Bipolar Pitch Env** (fixed, 2026-10-20): from the general matrix, the Bipolar envelope rose
  to 1 and stayed at `S`. The 352 presets with the default setup played an octave up. Now it
  follows the model in [05](05-envelopes-lfos.md#types-type).
  - Verified by a host render with the same note sequence: the code before the new ladder, with
    this fix, gives a Rhodes identical to the first version with samples (correlation 1.00).
    Moving to the content image did not change the sound.
  - With the decompiled ladder, the Rhodes correlates between 0.58 and 0.86 with that version:
    it is somewhat brighter and about 2 dB louder at high velocity.
- **Matrix at note start** (fixed): the envelopes advance with the previous block's matrix, and
  a free voice had it at 0. A short attack finished in the first block without seeing the
  modulation, and the Rhodes' Knob 1 → Amp Attack did nothing. Now the first block evaluates the
  matrix once before advancing the envelopes, with the envelopes and LFOs at 0.
- In mono, a note-on plays legato only if the previous key is still held. That way, an AD
  envelope that is still sounding does not prevent retriggering the note.
- **Velocity layers and splits** (`Osc` `Layer`/`Split 2`/`Split 3` with `LL1..LX2`): the
  per-oscillator gains decompiled from `cOscTriple`
  ([03](03-oscillators.md#velocity-layers-and-splits-osc--cosctriple-b)), computed at note-on
  and on mono legato. An oscillator with gain 0 is not rendered.

The wavetables are converted with [`firmware/tools/tbl2h.py`](../../firmware/tools/tbl2h.py):
- It takes one mip per octave out of the `.tbl`'s 44, resamples it to 1024 and stores int16 in
  flash.
- Orchestral: 6 frames, ~135 KB.
- The generated header comes from NS2 content: it is for local use, not for distribution.

Translation of **Abandoned Factory**:

| In the preset | In the port |
|---|---|
| Osc1 WT `Instrument/Orchestral` Pos 0.5, Trn +7, Lev 0 | same; it only feeds Osc2's ring |
| Osc2 Analog Sine, Trn −24, Lev 2, `Ring` 1 | same |
| Osc3 WT Orchestral Pos 0.5, Trn −24, Lev 0.73 | same |
| F1 LP 12-D, `CutO` 0.255, `KeyT` 0, `OGn` 1.34 + Filter Env (Dept 100) | same |
| EnvA/EnvF with curves and `Levl`; EnvA `VLev` 0.4, `KLev` 0.4 (`KCtr` 24) | same |
| LFO3 (36.6 Hz), LFO4 (0.15 Hz) → cutoff; LFO5 (0.28 Hz) → WT position of Osc1/Osc3 | same |
| F3 Comb Neg, `CutO` 0.259, `KeyT` 1 + LFO5 → cutoff (Dept 100) | global comb |
| `Uni` 2, `UDtn` 0.12, `USte` 0.87 | unison ×2: ±2 cents, pan 0.07 / 0.93 |
| Flanger, Delay and Reverb with `Mix` 0 | off |

What was left out:

- `Rout=STEREO` with F2 off: the port runs both channels through F1.
- Per-oscillator pan (LFO4 → Osc2 pan).
- LFO5 → Osc2 pulse width: it is a sine, it has no pulse width.
- LFO3 → Osc3 phase and `POff`.
- The pitch envelope (Bipolar with S = 1).
- The preset's knobs: in the port, the Korg's knobs keep their own map.

Measured load: 10 voices (5 notes × unison 2) use ~76 % of core 1.

### Content image: presets, wavetables and samples in flash

NS2's presets travel as data, together with the wavetables and samples they use, in a `content`
flash partition. The firmware reads it with `esp_partition_mmap`, through the cache, without
copying it ([`content.h`](../../firmware/obsidian_esp32/content.h)).

- [`content2bin.py`](../../firmware/tools/content2bin.py) builds the image from presets or preset
  folders:
  - It runs `obs2preset.py` and stores each assignment of the generated C++ as a
    `(field, value)` record. The field goes into a shared path table
    (`osc[0].fm.env[3].decay`).
  - It adds the wavetables the presets use (converted with `tbl2h.py`) and the samples of the
    presets named with `--samples`.
  - It leaves out, and lists, the presets with an oscillator type the engine lacks (NanoSaw,
    PD), those that use samples without `--samples` and those that use NanoStudio's own samples
    (`/AppRes`, 8.1 MB that do not fit).
- The firmware resolves each path with a registry of the `Patch` fields (`syn::forEachField` in
  `synth.h`). To load a preset, it builds the default `Patch` and applies the records in order.
  On the Mac, a preset built this way matches the same preset compiled as C++ in all 818 fields.
- Categories per pad:
  - First the `Tags` bits: 19 drums, 2 bass, 0 lead, 1 pad and 10 keys.
  - Before that, the words "pluck" and "brass/string" in the name, because `Tags` has no such
    categories.
  - If nothing applies, other keywords in the name and, finally, "Other".
- [`partitions_content.csv`](../../firmware/tools/partitions_content.csv) (copied as the sketch's
  `partitions.csv`): a 2 MB app at `0x10000` and a 14.2 MB `content` partition at `0x210000`.
  There is no FAT any more.

With the 312 factory presets and the 217 user presets, plus the Rhodes samples, the image holds
448 presets, 23 wavetables and 57 samples: 13.8 MB, 95 % of the partition. 81 are left out, all
because of samples: 73 user presets (they would have to be named with `--samples`, and not all
of them fit) and 8 factory presets with NanoStudio's own samples. To build and write it:

```bash
python3 -I firmware/tools/content2bin.py content.bin "/Applications/NanoStudio 2.app/Wrapper/NanoStudio 2.app/Factory/Instruments/Obsidian" "<preset folder>" --samples "<folder>/RhodesMK2Raw.obs"
esptool --chip esp32s3 --port /dev/cu.usbmodem21201 write-flash 0x210000 content.bin
```

The image only needs rewriting when the presets change; the firmware is not recompiled. It is
content derived from NS2 and from the presets' samples: local use.

On the board, presets 0..7 are my own and the image's follow from 8.
- `list [pad]` and `find <text>` show them over serial.
- Each Korg pad cycles through its category.
- On a preset change, the parameters that were moved (knobs, CC, serial) are applied again, and
  the knobs that move a macro of the new preset take its physical position.

On the Mac, 7 of the 448 presets are silent with one note at velocity 100: Doppler's Bike (comb
as a voice filter), Electric Bubbles, Synced Noises, BA 3, PD Chimes, PD Deeper Chimes and PD
Keys or Pad (analog sync, or levels that only arrive through macros).

Measured on the board with the previous, samples-only image: the 9.3 MB are mapped whole (the
S3's external address space is 32 MB, shared with the app and the 8 MB of PSRAM). With 10 voices
at velocity 80 (two Sample oscillators per voice, in the crossfade) core 1 sits at ~63 %. Each
active oscillator reads about 96 KB/s sequentially from flash.

Translation of **RhodesMK2Raw**:

| In the preset | In the port |
|---|---|
| 3 Sample oscillators (ROS/ROM/ROH, 18–20 zones), `Trn` 1, `Dtn` ≈ 0.47, `Lev` 0.44/1.52/2 | same |
| `Osc` `Split 3`: `S3L1` 0.353, `S3L2` 0.8745, `LX1` 1, `LX2` 0.0325 | same |
| F1 LP 24-A, EnvA/EnvF | same |

What was left out (`obs2preset.py` lists it): `Rout=PARALLEL`, the matrix routes from velocity,
pitch envelope, pitch wheel, Pad X/Y and knobs, `GTim` in poly and the Flanger.

### Drum presets

These 8 factory presets were the first drum presets (previously compiled into the firmware; now
they are in the content image, on pad 7):

| Preset | What is missing (besides knobs, XY pads and pitch wheel) |
|---|---|
| Goa kick | F1 EQ Shelf, F2 EQ Peak, F3 WS Tube |
| Hardcore Kick | F1/F3 WS Hard, F2 EQ Peak; `Poly=0` played poly |
| x0x Snare | Brown noise, `Drv`, F2 EQ Peak, global F3 HP 12-A |
| Snare 5 | `Drv`, STEREO routing, F3 WS Hard |
| x0x Hat 1 CL / OP | `Drv`, `Brk`/`D2`, F2 EQ Shelf, global F3 LP 12-A, Env 5 → Reverb Mix (the reverb stays at `Mix` 1) |
| Dirty Clap | `Drv`, `FMix`, F3 HP 12-A, Filter Env → Noise Cutoff, Saw Down LFO |
| Electro Perc | F2 WS Soft, F3 EQ Peak, velocity routes |

Measured on the host (note 60, velocity 110): the kicks sweep down to ~60 Hz in 100 ms, and every
voice ends on its own through the AD (from 27 ms to 900 ms; Electro Perc is short because its
`Velocity → Amp Env Decay` subtracts in the `x` domain). On the board, a drum voice uses 5 to
10 % of core 1. It was not compared against an NS2 render.

### Preset coverage (2026-10-20, with Drive and the decompiled ladder)

Result of `obs2preset.py` on the 312 presets in `Factory/` and the 217 user and third-party
presets, with the same criteria as before. Whatever does not change the sound at rest is not
counted: macros at 0, pitch wheel, `Rout` without F2, voice count, default `GTim` in poly,
`Rand → Phase` and the reverb type. 25 factory and 26 user presets are complete (before Drive it
was 6 and 14).

No oscillator type and no `Drv` are missing any more. With BP and Notch on the ladder, F2 went
down from 45/27 to 15/9 and the non-comb global filter from 30/3 to 22/0. The delay is no longer
missing: the 147/71 that flag it are tempo-synced delays played at 120 BPM.

| Missing | Factory (312) | User (217) | Comment |
|---|---|---|---|
| Tempo (LFOs and delay at 120 BPM) | 131 / 147 | 48 / 71 | the port has no transport |
| Per-oscillator pan and width | 93 | 16 | |
| Chorus / Ensemble / Flanger | 62 | 54 | |
| Sync, Pulse/Bi-Pulse | 46 / 45 | 61 / 25 | |
| Per-oscillator phase offset | 45 | 58 | |
| Envelope breakpoint and delay | 39 | 39 | |
| Missing filter types (Formant, Lo-Fi, LP 6-D, Comb on F1/F2) | 50 | 23 | |
| Macros to FX (Multi-FX Mix, Reverb Time) | 33 | 57 | |

### Performance and memory with FM (2026-10-20)

- `synth.h` compiles with `-O2` (`#pragma GCC optimize`) instead of the sketch's `-Os`. With 10
  voices, Poly went from 75 % to 65 % of core 1 and Abandoned Factory from 87 % to 68 %.
- An FM oscillator costs ~7,200 cycles per block with two active operators and ~10,400 with all
  three (a wavetable oscillator, ~1,750). Tables in flash or in RAM perform the same.
- FM and the matrix made the voice bigger, and when BLE started there was no internal RAM left
  (the board rebooted in a loop). The analog shape bank (`WaveBank`, ~90 KB) moved to PSRAM:
  183 KB of internal RAM are left free, and Poly's load did not change measurably.
- **Overload protection**: if the voice load, averaged over ~16 blocks, goes over 90 %, the
  oldest voice is killed (`stat` counts the killed ones). Without this, a heavy preset starves
  the `loop()` on the same core of CPU, note-offs do not arrive and the board hangs. With
  10-note chords, the FM presets settle between 6 and 10 voices (79 % to 87 %); the others do
  not lose voices.

- **Unstable phaser with `Dep` 1** (fixed): the sweep `150·2^(8·Dep)` reached 38 kHz. Past
  Nyquist the allpass diverges even if `Mix` is 0. The NaN stayed in the phaser and in the
  plate, and from then on every preset with reverb came out silent until a reboot. Now the sweep
  is clamped to `0.45·sr`. `stat` shows the output peak since the last `stat`, to catch this
  without listening.

The first FM presets tested (now in the content image):

| Preset | Pad | What is missing |
|---|---|---|
| Electric Bass, Bass Organ | 1 Bass | PARALLEL routing with F2 off, a macro to Multi-FX Mix, `Widt` |
| FM Stoned Keys, Electric Piano 2, Organ FM, Bell | 8 Keys | envelope `Brk`, per-oscillator pan, `Drv`, macros to FX |
| Mcp Dance Pluck (user) | 4 Pluck | delay |

Organ FM uses the `Complex/Tech` wavetable.

**Natural next steps:**

- Per-oscillator pan and stereo width (including the NanoSaw's `Widt`), chorus/ensemble.
- Confirm what is marked [I] and [?] against NS2 renders, especially the Pitch Coarse scale,
  Amp Gain, the delay and the PD shapes other than the saw.

## Verification against NS2

For each module: a minimal patch in NS2 (one oscillator, no filter or FX), a render of a fixed
note and a comparison of spectrum and envelope against the port. The questions marked **R** in
[09-open-questions.md](09-open-questions.md) are resolved this way.

### Reference renders on the Mac (2026-10-20)

NS2 2.1.2 runs on the Mac (Apple Silicon). Each measurement follows these steps:
1. **MIDI**: a virtual CoreMIDI port ("ObsRef") plays a fixed, timestamped sequence: C3, C4,
   C5, C6 and C2, notes at velocity 40/90/127 and a chord.
2. **Audio**: the output goes to the Loopback device "Master" and is recorded with AudioQueue at
   48 kHz. Capturing with ffmpeg through avfoundation lost ~10 % of the samples.
3. **Presets**: the test patches are written as `.obs` to `Documents/Instruments/Obsidian/User`
   in NS2's container, and loaded after Manage → Refresh. NS2 only reacts to slow taps (press,
   move, release).
4. **Comparison**: the NS2 take against the port's host render, before the volume and the
   limiter, aligned by envelope correlation. Per note, level, envelope, octave bands in the
   sustain and tuning are measured.

Findings fixed with this:
- The amplitude is the square of the envelope ([05](05-envelopes-lfos.md)).
- The analog shapes have the RMS of a sine, like `Analog.tbl`: saw and tri peak at 1.22 and the
  square at 0.71 ([03](03-oscillators.md)).
- EQ Shelf works on the highs, the ladder saturates at twice the level, and a disabled F1 does
  not filter ([04](04-filters.md)).
- `Poly=0` is mono ([02](02-preset-format.md)).
- EQ Shelf is a low shelf with slope `Q²` and overall gain `1/A`, that is a tilt EQ
  ([04](04-filters.md)). EQ Peak, Output Gain, SERIES routing and basic FM (carrier, 2→3, 1→3
  with ratio 2, feedback) match NS2.
- The Bipolar Pitch Env (fixed earlier) and the matrix evaluation: the envelopes advance with
  the previous block's matrix, as the decompilation says.
- `Autm` C02..C11 are Pad X, Pad Y and Knob 1..8, in the order of the controls' names
  ([02](02-preset-format.md#autm)). They used to be read as Knob 1..8 and Pad X/Y, which left
  dark or silent the presets that open the filter with Pad X.
- The saw starts at 0 and rises, and the note starts without a fade-in
  ([03](03-oscillators.md), [05](05-envelopes-lfos.md)).
- In the FM connections, `Levl` 0 leaves the envelope active ([03](03-oscillators.md)).
- Retrigger and the synced Random Loop ([05](05-envelopes-lfos.md)).
- Unison level counts squared ([02](02-preset-format.md#unison-b)). `Lev` 2 doubles the
  amplitude, and the ladder's Drive with two oscillators matches.
- Waveshapers: the Cutoff is the dry/processed mix and `WSGn` does nothing
  ([04](04-filters.md#formant-waveshapers-and-lo-fi-b)).
- A free voice starts with its filters at zero: without a fade-in, the state left over from the
  previous note sounded like a click at the start.
- Open: the FM envelope tail with `Levl` 0 ([03](03-oscillators.md)).
- Effects measured and fixed: delay (fixed dry, `Mix²` wet, Cross/Pingpong, EQ), reverb (mix
  law, decay and level per type, Freeverb for the Rooms, HDmp, cutoffs, pre-delay),
  chorus/flanger (new) and phaser ([07](07-effects.md)).
- NanoSaw: the centre saw at 0.88 and the side saws at 0.52 ([03](03-oscillators.md)).
  Formant-M/F implemented ([04](04-filters.md)).
- Unison pan with constant sum, as in NS2 ([02](02-preset-format.md#unison-b)). Three-level
  Bi-Pulse, and `Osc N Phase` as a matrix destination (it shifts the oscillator's phase; with
  `Rand` each note starts at a different phase) **[I]**.
- Open: Fat Wide Pad (3 FM oscillators with feedback) comes out ~13 dB louder than NS2, although
  a single oscillator matches; Africa Brass and other presets with unison 2 and high `USte` are
  2–4 dB loud with the constant-sum law, which the direct test confirms.

With that, the 17 single-module test patches match NS2 in level (±0.3 dB between tests), tuning
(0 cents) and bands (±5 dB): saw, square, sine, tri, LP24 with and without Q, LP12 and LP24 with
Drive, LP12-D, HP24, filter envelope, amplitude envelope, velocity and Levl.

The tools are in [`firmware/tools/ref`](../../firmware/tools/ref) (`build.sh` compiles, `rec.sh`
records and `gen.py` builds the test patches). The sequence lives in `seq.h`.

For the effects there is a second sequence of short notes 5 s apart (`kFxSeq` in `seq.h`):
`rec.sh <take> fxseq` plays it in NS2 and `render <image> <preset> <output> fx` in the port, so
the delay and reverb tails are measured without overlapping ([07](07-effects.md)).

`rank.py` (needs numpy) renders each take `<prefix>NN <preset>.raw` with the port and sorts the
presets by distance to NS2: overall gain, mean difference of the 10 ms envelope and of the
1/3-octave spectra of the first second of each note. The reference gain is −5.6 dB (NS2 comes
out lower than the port's render before the volume). The takes are recorded in batches of 9 (the
visible rows of NS2's list), with the recorder and the taps synchronized by clock in 30 s slots.

Status of the 37 recorded factory presets (after the fixes above):
- Matching (envelope and spectrum ≤ 3 dB, level ±2 dB): Deep Bass, Bottle Bass, FM Stoned Keys,
  Rave Lead, Bold as Brass, Africa Brass, Percussive Bass, Hardstyle Lead, Hang Drum, Hard
  Glider, Snare 4, NanoSaw 80s, Analogue Kick and Classic Acid (except the spectrum).
- Right shape, different level: the port comes out 3–9 dB louder on Fat Wide Pad, Iron Drops,
  Ocean of Secrets, Electric Piano 2, Slow Sweep Pad and Magic Pitch, and 5–9 dB lower on Pluck
  or Pad, Dubstep Crush, 7th House and Dirty Saws.
- With FX or modulations the port lacks: PsyDelay (Env 4 → Delay Time/FB), Arping On (the
  Random Loop pattern), Dead Space and Liquid Resonance.
- The "spectral" difference of some FM presets is NS2's noise floor (−85 to −100 dBFS on even
  harmonics), which is inaudible.
