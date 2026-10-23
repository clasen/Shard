// Minimal Obsidian-style synth engine for ESP32-S3 (see docs/obsidian for the formulas).
#pragma once

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <type_traits>
#include "fm_waves.h"

// The engine runs in the audio loop: compile it with -O2 instead of the sketch's -Os.
#pragma GCC push_options
#pragma GCC optimize("O2")

namespace syn {

constexpr float kSampleRate = 48000.0f;
constexpr int kBlock = 64;
constexpr int kVoices = 10;
constexpr int kTableLen = 1024;
constexpr int kMips = 11;
constexpr float kMipBaseHz = 20.0f;

enum Wave { kSine, kTri, kSaw, kSquare, kPulse, kBiPulse };
constexpr int kWaveCount = 4;  // band-limited tables; the pulse is read from the saw's
enum OscType { kOscWave, kOscTable, kOscNoise, kOscSample, kOscFM, kOscNanoSaw, kOscPD };
enum PdWave { kPdSaw, kPdSquare, kPdPulse, kPdDoubleSine, kPdSawPulse, kPdResSaw, kPdResFatSaw, kPdResTri, kPdResTriHP };
enum VelSplit { kVelLayer, kVelSplit2, kVelSplit3 };
enum NoiseType { kNoiseWhite, kNoiseLowPass, kNoiseHighPass, kNoiseBandPass, kNoiseNotch };
enum FilterType { kFilterLP24A, kFilterLP12D, kFilterLP12A, kFilterHP12A, kFilterHP24A, kFilterHP12D,
                  kFilterBP6A, kFilterBP12A, kFilterNotch6A, kFilterNotch12A, kFilterOff,
                  kFilterEQPeak, kFilterEQShelf, kFilterFormantM, kFilterFormantF,
                  kFilterWSTube, kFilterWSSoft, kFilterWSHard, kFilterWSEnhance, kFilterWSSine, kFilterWSTri,
                  kFilterCombPos, kFilterCombNeg };
constexpr int kShapes = 6;         // WS Tube .. WS Tri
constexpr int kShapeLen = 1025;    // NS2's waveshaper tables: a ramp from -1 to 1 through the curve
enum EnvType { kEnvADSR, kEnvAD, kEnvBipolar };
// Filt/Rout, in NS2's enum order (0x1004036c0): PARALLEL, SERIES, STEREO (F2 runs F1's settings).
enum FilterRoute { kRouteParallel, kRouteSeries, kRouteStereo };
enum LfoType { kLfoSine, kLfoSquare, kLfoTriangle, kLfoSawUp, kLfoSawDown, kLfoSawDouble, kLfoRandom, kLfoRandomLoop };
enum LfoOut { kLfoBipolar, kLfoUniHalf, kLfoUniFull };
enum LfoSync { kLfoFree, kLfoKey, kLfoGlobal };
constexpr int kLfos = 5;
constexpr int kEnvs = 5;    // Amp, Filter, Pitch, Env 4, Env 5
constexpr int kMacros = 10; // Knob 1..8, Pad X, Pad Y
constexpr int kModSlots = 24;

// Mod-matrix sources (06-modulation.md). Per voice up to kSrcLfo1 + kLfos; global from kSrcPitchWheel.
enum ModSrc : int8_t {
  kSrcNone, kSrcConst, kSrcRand1, kSrcRand2, kSrcNote, kSrcVelocity, kSrcGate,
  kSrcEnv1,                          // Amp Env; Filter, Pitch, Env 4, Env 5 follow
  kSrcLfo1 = kSrcEnv1 + kEnvs,
  kSrcPitchWheel = kSrcLfo1 + kLfos, kSrcPitchWheelUp, kSrcPitchWheelDown,
  kSrcMacro1,                        // Knob 1..8, then Pad X, Pad Y
  kSrcCount = kSrcMacro1 + kMacros
};

constexpr ModSrc srcEnv(int i) { return (ModSrc)(kSrcEnv1 + i); }
constexpr ModSrc srcLfo(int i) { return (ModSrc)(kSrcLfo1 + i); }
constexpr ModSrc srcMacro(int i) { return (ModSrc)(kSrcMacro1 + i); }

// Mod-matrix destinations. `index` in a slot picks the oscillator (3 = all), envelope or LFO.
constexpr int kFmConns = 8;  // O11, O12, O13, O22, O23, O1O, O2O, O3O (the engine's order)
enum ModDest : int8_t {
  kDstOscPitch, kDstOscFine, kDstOscLevel, kDstOscRing, kDstWtPos, kDstNoiseCut, kDstNoiseQ, kDstNoiseDeci,
  kDstFmConn,                          // FM connection amounts, kFmConns of them
  kDstFmRatio = kDstFmConn + kFmConns, // FM operator ratios (FM OPn Freq), 3 of them
  kDstNsDetune = kDstFmRatio + 3, kDstNsMix, kDstPdDist,  // NanoSaw Detune/Mix, PD Phase Dist
  kDstPulseWidth,                      // An Pulse Width
  kDstOscPhase,                        // Phase: shifts the running oscillator, 1 = a full cycle
  kDstCutoff, kDstCutoffSlow, kDstQ, kDstFilterGain, kDstDrive,
  kDstAmpGain,
  kDstCutoff2, kDstQ2, kDstFilterGain2, kDstDrive2,  // Filter 2
  kDstEnvAttack, kDstEnvDecay, kDstEnvSustain, kDstEnvRelease, kDstEnvLevel, kDstEnvRetrigger,
  kDstLfoRate, kDstLfoLevel,
  kDstGlobalCutoff, kDstGlobalQ, kDstReverbMix, kDstMultiFxMix, kDstDelayMix  // engine-wide
};
constexpr int kOscDests = kDstCutoff;
constexpr ModDest dstFmConn(int i) { return (ModDest)(kDstFmConn + i); }
constexpr ModDest dstFmRatio(int i) { return (ModDest)(kDstFmRatio + i); }

// Scales from the matrix sum m to each destination's units. Pitch Coarse/Fine in semitones:
// the factory presets bend with "Pitch Wheel -> Pitch Coarse, Dept 2" [I].
constexpr float kPitchCoarseRange = 100.0f, kPitchFineRange = 1.0f;
// Master trim before the soft clip (+2 dB): keeps the loudness the board had before the amp envelope was
// squared like NS2's, which made the mix about 5 dB quieter [I].
constexpr float kOutputTrim = 1.26f;
// Tempo of the tempo-synced LFO rates and delay times: MIDI clock or the "bpm" command.
inline float gBpm = 120.0f;
constexpr float kClipKnee = 0.9f;  // the output soft clip is linear below this level; the limiter keeps under it

// ---------------------------------------------------------------------------
// Band-limited wavetables, one mip per octave starting at kMipBaseHz.
// ---------------------------------------------------------------------------
struct WaveBank {
  int16_t table[kWaveCount][kMips][kTableLen + 1];
  float scale[kWaveCount][kMips];  // gain that brings each table to the RMS of a sine, like NS2
  float shaper[kShapes][kShapeLen];  // waveshaper filter types

  // NS2's waveshaper curves (0x1000fd848) over x in [-1, 1]. The circular knees join a straight
  // segment to a flat top: y = c + sqrt(r2 - (min(|v|, e) - e)^2) past the knee point.
  static float shape(int s, float x) {
    auto knee = [](float a, float e, float r2, float c) { const float d = fminf(a, e) - e; return c + sqrtf(r2 - d * d); };
    switch (s) {
      case 0: {  // Tube: a parabola, then soft tops on both sides
        float y = 0.5f * (x + 1.41f) * (x + 1.41f) - 1.0f;
        if (y > 0.9f) y = knee(y, 1.1413f, 0.11655f, 0.6587f);
        else if (y < -0.3f) y = 1.3891f - sqrtf(5.7077f - (fminf(-y, 1.9891f) - 1.9891f) * (fminf(-y, 1.9891f) - 1.9891f));
        return y;
      }
      case 1: {  // Soft: 1 - 2 / (e^(18.98x) + 1) = tanh(9.49x)
        const float v = fminf(fmaxf(x * 18.984f, -10.0f), 10.0f);
        return 1.0f - 2.0f / (expf(v) + 1.0f);
      }
      case 2: {  // Hard: slope 15 with a knee at 0.8
        const float v = x * 15.0f, a = fabsf(v);
        return a <= 0.8f ? v : copysignf(knee(a, 1.2826f, 0.46594f, 0.31741f), x);
      }
      case 3: {  // Enhance: cubic crossover below 0.1, then different gains and knees per side
        float y = fabsf(x) < 0.1f ? 100.0f * x * x * x : x;  // (10|x|)^2 * x
        if (y >= 0.0f) {
          const float v = y * 1.7065f;
          y = v <= 0.5f ? v : knee(v, 1.7065f, 2.912f, -0.70648f);
        } else {
          const float v = y * 1.1413f;
          y = -v <= 0.9f ? v : -knee(-v, 1.1413f, 0.11655f, 0.6587f);
        }
        return y;
      }
      case 4: return sinf(13.5f * x);          // Sine: a sine folder
      default: return asinf(sinf(13.5f * x));  // Tri: a triangle folder
    }
  }

  void buildShapers() {
    for (int s = 0; s < kShapes; s++) {
      float* t = shaper[s];
      float peak = 0.0f;
      for (int i = 0; i < kShapeLen; i++) {
        t[i] = shape(s, -1.0f + (float)i / 512.0f);
        peak = fmaxf(peak, fabsf(t[i]));
      }
      const float g = peak > 1e-6f ? 1.0f / peak : 0.0f, centre = t[512] * g;
      for (int i = 0; i < kShapeLen; i++) t[i] = t[i] * g - centre;  // normalized, DC removed
    }
  }

  void build() {
    static float sinLut[kTableLen];
    static float acc[kTableLen];
    for (int i = 0; i < kTableLen; i++) sinLut[i] = sinf(6.28318531f * i / kTableLen);
    for (int w = 0; w < kWaveCount; w++) {
      for (int m = 0; m < kMips; m++) {
        float topHz = kMipBaseHz * (float)(2 << m);
        int harmonics = (int)(20000.0f / topHz);
        if (harmonics > kTableLen / 2 - 1) harmonics = kTableLen / 2 - 1;
        if (harmonics < 1) harmonics = 1;
        memset(acc, 0, sizeof(acc));
        for (int k = 1; k <= harmonics; k++) {
          float amp;
          switch (w) {
            case kSine: amp = k == 1 ? 1.0f : 0.0f; break;
            case kTri: amp = (k & 1) ? ((((k - 1) / 2) & 1) ? -1.0f : 1.0f) / (float)(k * k) : 0.0f; break;
            case kSaw: amp = ((k & 1) ? 1.0f : -1.0f) / (float)k; break;  // rising from 0, like NS2
            default: amp = (k & 1) ? 1.0f / (float)k : 0.0f; break;
          }
          if (amp == 0.0f) continue;
          for (int n = 0; n < kTableLen; n++) acc[n] += amp * sinLut[(k * n) & (kTableLen - 1)];
        }
        float peak = 1e-9f, energy = 0.0f;
        for (int n = 0; n < kTableLen; n++) { peak = fmaxf(peak, fabsf(acc[n])); energy += acc[n] * acc[n]; }
        for (int n = 0; n < kTableLen; n++) table[w][m][n] = (int16_t)(acc[n] / peak * 32767.0f);
        table[w][m][kTableLen] = table[w][m][0];
        // NS2's Analog.tbl frames all have the RMS of a full-scale sine (0.7071): saw and tri peak at
        // 1.22, the square at 0.71.
        scale[w][m] = peak * 0.70710678f / sqrtf(energy / kTableLen);
      }
    }
    buildShapers();
  }

  static inline int mipFor(float hz) {
    int m = 0;
    float top = kMipBaseHz * 2.0f;
    while (m < kMips - 1 && hz >= top) { top *= 2.0f; m++; }
    return m;
  }
};

extern WaveBank* gBank;

// An imported wavetable (firmware/tools/tbl2h.py): frames x octave mips, int16 with one scale.
struct Wavetable {
  int frames;
  const int16_t (*data)[kMips][kTableLen + 1];
  float scale;
};

// A sample of the flash image (firmware/tools/samples2bin.py): mono int16 at its own rate.
struct Sample {
  const int16_t* data;
  uint32_t length, loopStart, loopEnd;  // loopEnd is exclusive; 0 = no loop
  float rate;                           // file sample rate / kSampleRate
};

// One key zone of a Sample oscillator (Samp/Zones/Zone). `sample` is bound by file name
// when the preset is loaded and stays null when the image does not hold the file.
struct SampleZone {
  const char* file;
  uint8_t lo, hi, root;
  float tune, vol;  // Tune in semitones [?], Vol linear
  bool loop;        // Loop=On: forward loop from the WAV's smpl chunk
  const Sample* sample;
};

// A Slate kit (NS2's sample drum machine) as content2bin.py stores it; the firmware reads it in
// place from the content image. Each pad layers up to 3 one-shot samples through a filter and an
// envelope. Times in seconds, pitches in semitones, levels linear [I].
constexpr int kKitPads = 32;
constexpr int kPadSamples = 3;
struct KitSample {
  int32_t sample;  // index into the image's samples, -1 = none
  float level, pan, pitch, offset;  // offset: start position, 0..1 of the sample
  uint8_t reverse, unused[3];
};
struct KitPad {
  uint8_t note, group, filterType, filterOn;  // filterType: 0 LP 6, 1 LP 12, 2 HP 6, 3 HP 12, 4 BP
  float level, pan;
  float attack, hold, decay, breakLevel, decay2, velLevel;  // decay, decay2 < 0: never
  float pitch;
  float cutoff, q, cutoffVel;  // cutoff 0..1 as Obsidian's; velocity opens it by cutoffVel [P]
  // The pad's FX bus: its filter (types as above) and its send to the kit's reverb.
  uint8_t busFilterType, busFilterOn, unused[2];
  float busCutoff, busQ, send;
  KitSample s[kPadSamples];
};
struct Kit {
  char name[40];
  uint8_t groupPoly[kKitPads];  // voices allowed per pad group (group 0 and 0 = no limit)
  KitPad pad[kKitPads];
};
static_assert(sizeof(KitSample) == 24 && sizeof(KitPad) == 140 && sizeof(Kit) == 4552, "content2bin.py's kit layout");

// ---------------------------------------------------------------------------
// Patch: a subset of the Obsidian parameters, in the same units as the .obs XML.
// ---------------------------------------------------------------------------
struct EnvPatch {
  EnvType type = kEnvADSR;
  float startLevel = 0.0f, releaseLevel = 0.0f;  // SLv/RLv: Bipolar start and release end levels
  float attack = 0.005f, decay = 0.4f, sustain = 0.6f, release = 0.4f;  // seconds / level
  float ac = 1.22051f, dc = 1.22051f, rc = 1.22051f;  // AC/DC/RC curve coefficients k (2 = linear)
  float level = 1.0f;  // Levl, scales the envelope output
  float vTim = 0.0f, vLev = 0.0f;  // VTim/VLev: velocity scaling of times and level, -1..1
  int kCtr = 24;                   // KCtr: note where key scaling is neutral
  float kTim = 0.0f, kLev = 0.0f;  // KTim/KLev: key scaling of times and level, -1..1
};

// FM oscillator (FM, cOscTypeFM): 3 operators and 8 connections, each with its own envelope.
struct FmPatch {
  uint8_t wave[3] = {0, 0, 0};             // operator Wave, as an index of fm_waves.h
  float ratio[3] = {1.0f, 1.0f, 1.0f};     // Rat * 0.0001
  float freqOffset[3] = {0.0f, 0.0f, 0.0f};  // FrqO, Hz
  float amount[kFmConns] = {};             // Amt, in the order of kFmConns
  EnvPatch env[kFmConns];                  // Levl is the envelope's depth: 0 = Amt only
};

struct OscPatch {
  bool enabled = true;
  OscType type = kOscWave;
  Wave wave = kSaw;
  const Wavetable* table = nullptr;  // kOscTable
  float position = 0.0f;             // Wave Pos, 0..1 across the frames
  float ring = 0.0f;                 // Ring: 0..1 blend towards osc(n-1) x osc(n)
  float keyTrack = 1.0f;  // KeyT: pitch = 60 + (note - 60) * KeyT
  int transpose = 0;   // Trn, semitones
  float fine = 0.0f;   // Dtn, semitones
  float level = 0.6f;  // Lev, linear 0..2
  float filterMix = 0.0f;  // FMix: share of the oscillator sent to the F2 bus instead of F1
  float phaseOffset = 0.0f;  // POff: the phase each note starts at, in cycles
  float pulseWidth = 0.5f;   // PWM (Analog Pulse)
  // Noise (Nois): Type, Cut, Q, Deci, plus the mod-matrix route Amp Env -> Noise Q.
  NoiseType noiseType = kNoiseWhite;
  float noiseCut = 0.5f, noiseQ = 0.0f, noiseDeci = 0.0f;
  // Sample (Samp): key zones, the first one containing the note plays.
  SampleZone* zones = nullptr;
  int zoneCount = 0;
  FmPatch fm;
  // NanoSaw (NS): Mix, FlOf, Dtn, Sprd. PD: Wave, PD.
  float nsMix = 0.75f, nsFalloff = 0.0f, nsDetune = 0.25f, nsSpread = 0.75f;
  PdWave pdWave = kPdSaw;
  float pdAmount = 0.5f;
};

// Velocity and key scaling taken at note-on (decompiled from NS2, 05-envelopes-lfos.md). `time`
// multiplies the stage times.
struct EnvScale {
  float level, time;
};
static EnvScale envScale(const EnvPatch& p, int note, float vel) {
  const float n = (float)note, c = (float)(p.kCtr > 0 ? p.kCtr : 1);
  float keyTime = 0.0f, key = 1.0f;
  if (n > c) {
    if (p.kTim > 0.0f) keyTime = (n - c) * p.kTim / c;
    if (p.kLev > 0.0f) key = 1.0f + (c - n) * p.kLev / (128.0f - c);
  } else {
    if (p.kTim < 0.0f) keyTime = (n - c) * p.kTim / c;
    if (p.kLev < 0.0f) key = 1.0f + p.kLev * (1.0f - n / c);
  }
  const float velTime = p.vTim * (64.0f / 127.0f - vel);
  const float velLevel = p.vLev > 0.0f ? 1.0f + p.vLev * (vel - 1.0f) : 1.0f + p.vLev * vel;
  // NS2 multiplies the envelope's step by 2^(3*(velTime + keyTime)) (0x1005316ac): positive KTim
  // makes the notes above KCtr faster.
  return {velLevel * key, exp2f(-3.0f * (velTime + keyTime))};
}

struct LfoPatch {
  float rate = 4.0f;  // Rt, Hz
  float beats = 0.0f;  // a tempo rate's period in beats (RtBt/BRt of a bar), 0 = Rt in Hz
  float ampl = 0.0f;  // Ampl, 0..1
  LfoType type = kLfoSine;
  LfoOut out = kLfoBipolar;  // OutT
  LfoSync sync = kLfoFree;   // Sync: OFF (random phase per note), KEY (PhOf), GLOBAL (shared phase)
  float phaseOffset = 0.0f;  // PhOf
  float attack = 0.0f;       // Atk: linear fade-in from the note-on, seconds
  float curve = 0.5f;        // Crv: phase bias of the sine
  float warp = 0.5f;         // Warp: 0.5 = neutral
  int quantize = 0;          // Qtz: steps, 0 = off
  int loopPattern = 1, loopLength = 16;  // RPtn/RLen (Random Loop)
};

// One mod-matrix slot (Mod/Cnn): adds src x scale x depth to the destination.
struct ModSlot {
  ModSrc src = kSrcNone;
  ModSrc scale = kSrcNone;  // Sca: a macro that scales the connection, or none
  ModDest dest = kDstCutoff;
  int8_t index = 0;
  float depth = 0;          // Dept / 100
};

// Global filter (F3) of any type but comb: one filter per channel after the voice mix.
struct GlobalFilterPatch {
  bool enabled = false;
  FilterType type = kFilterLP24A;
  float cutoff = 1.0f, q = 0.0f, gain = 1.0f, shelfGain = 0.0f;  // CutO Q OGn SGan
  float drive = 0.0f;  // Drv
  float vowel = 0.5f;  // FVwl (Formant types)
};

// Global filter (F3) as a comb: Comb Pos / Comb Neg.
struct CombPatch {
  bool enabled = false, negative = false;
  float cutoff = 0.5f, q = 0.0f, mix = 1.0f;
};

// Delay (Dly, cEffectDelaySimple, 07-effects.md): two lines, the right one N times longer for the
// 1:N types [?]. Stereo feeds each line back into itself, Cross and Pingpong into the other.
enum DelayType { kDelayStereo, kDelayCross, kDelayPingpong };
struct DelayPatch {
  bool enabled = false;
  DelayType type = kDelayStereo;
  int ratio = 1;      // 1:N
  float time = 0.25f; // seconds of the left line
  float beats = 0.0f; // a tempo time in beats (TNum/TDen of a bar), 0 = time in seconds
  float feedback = 0.3f, eq = 0.5f, mix = 0.0f;  // FB, EQCt (0.5 = flat), Mix
};

// Multi-FX chorus/flanger (MFX Chorus/Ensemble/Flanger): Rat, Dep, Dly, FB, Wid, Mix in 0..1.
enum ChorusType { kChorus1, kChorus2, kEnsemble1, kEnsemble2, kFlanger1, kFlanger2 };
struct ChorusPatch {
  bool enabled = false;
  ChorusType type = kChorus1;
  float rate = 0.25f, depth = 0.5f, delay = 0.5f, feedback = 0.0f, mix = 1.0f;  // Wid is not played
};

// Multi-FX phaser (MFX Phaser 1/2): Rat, Dep, FB, Wid, Mix in 0..1.
struct PhaserPatch {
  bool enabled = false;
  int stages = 8;
  float rate = 0.25f, depth = 0.5f, feedback = 0.5f, width = 0.5f, mix = 1.0f;
};

struct Patch {
  OscPatch osc[3];
  // Osc block (cOscTriple): velocity layers/splits across the three oscillators.
  VelSplit velSplit = kVelLayer;      // Type: Layer, Split 2, Split 3
  float velLayer[2] = {0.0f, 0.0f};   // LL1/LL2: velocity where Osc 2/3 come in (Layer)
  float split2 = 0.5f;                // S2L1
  float split3[2] = {0.333f, 0.667f}; // S3L1/S3L2
  float velXFade[2] = {0.0f, 0.0f};   // LX1/LX2: crossfade widths, 0..1
  float cutoff = 0.35f;       // CutO, 0..1 -> 20*900^x Hz
  float resonance = 0.35f;    // Q, 0..1
  float keyTrack = 0.5f;      // KeyT, 0..2
  float filterEnvAmt = 0.35f; // normalized cutoff added at env peak
  FilterType filterType = kFilterLP24A;
  float filterGain = 1.0f;    // OGn
  float shelfGain = 0.0f;     // SGan, dB (EQ types)
  float filterPan = 0.5f;     // Pan of F1's output
  float drive = 0.0f;  // Drv of F1
  float vowel = 0.5f;  // FVwl of F1 (Formant types)
  float combMix = 1.0f;  // CMix of F1 (Comb types)
  FilterRoute route = kRouteStereo;
  bool osc1Bypass = false;    // O1BP=FILTER: Osc 1 skips both filters
  // F2, in the units of F1's fields.
  struct {
    bool enabled = false;
    FilterType type = kFilterLP24A;
    float cutoff = 1.0f, resonance = 0.0f, keyTrack = 1.0f, gain = 1.0f, shelfGain = 0.0f, pan = 0.5f;
    float envAmt = 0.0f;  // Filter Env -> Filter 2 Cutoff
    float drive = 0.0f;
    float vowel = 0.5f;
    float combMix = 1.0f;
  } f2;
  EnvPatch ampEnv, filtEnv, pitchEnv, env4, env5;
  LfoPatch lfo[kLfos];
  ModSlot mod[kModSlots];
  CombPatch comb;
  GlobalFilterPatch f3;
  int unison = 1;             // Uni, voices per note (1..4)
  float unisonDetune = 0.0f;  // UDtn, 0..1
  float unisonStereo = 0.0f;  // USte, 0..1
  float glide = 0.0f;
  float macro[kMacros] = {};  // knob and pad positions when the preset loads (Autm)
  bool mono = false;  // Poly 0 or 1: one note (in unison), legato notes glide without retriggering
  float reverbMix = 0.22f;
  // Reverb (07-effects.md): obs2preset turns Type/Decy/HDmp/LCut/HCut into these engine values.
  float reverbDecay = 0.55f;   // tank feedback of the plate, or the combs' feedback of the room
  bool reverbRoom = false;     // Small/Large Room: Freeverb combs instead of the Dattorro plate
  float reverbDamp = 0.3f;     // high damping in the loop, 0..1
  float reverbGain = 1.0f;     // wet level at Mix 1
  float reverbHpHz = 20.0f, reverbLpHz = 20000.0f;  // HCut / LCut on the wet signal
  float reverbPre = 0.0f, reverbPreMix = 0.0f;      // pre-delay (s) and its crossfade (PDel, PDMx)
  float reverbWidth = 1.0f;    // side gain of the wet signal (from Widt): 0 = mono, 1 = the engine's own stereo
  const Kit* kit = nullptr;    // a Slate kit: the notes play its pads instead of the voices
  const Sample* kitSamples = nullptr;  // the samples its pads index
  float volume = 0.5f;
  PhaserPatch phaser;
  ChorusPatch chorus;
  DelayPatch delay;

  // Every preset starts with the pitch wheel bending all oscillators by +-2 semitones.
  void clearMod() {
    for (ModSlot& m : mod) m = ModSlot();
    mod[0] = {kSrcPitchWheel, kSrcNone, kDstOscPitch, 3, 0.02f};
  }

  Patch() {
    clearMod();
    osc[0].wave = kSaw;
    osc[1].wave = kSaw; osc[1].fine = 0.08f;
    osc[2].wave = kSquare; osc[2].transpose = -12; osc[2].level = 0.4f;
    filtEnv.attack = 0.002f; filtEnv.decay = 0.35f; filtEnv.sustain = 0.15f; filtEnv.release = 0.3f;
    ampEnv.vLev = 1.0f;  // the built-in presets play the full velocity range
  }
};

// ---------------------------------------------------------------------------
// Every Patch field a preset loaded from the content image can set, named as obs2preset.py
// writes it ("osc[0].fm.env[3].decay"). Pointers (wavetables, sample zones) are set apart.
// ---------------------------------------------------------------------------
enum FieldKind : uint8_t { kFieldF32, kFieldI32, kFieldI8, kFieldU8, kFieldBool };

template <class T> constexpr FieldKind fieldKind() {
  return std::is_same<T, float>::value ? kFieldF32
       : std::is_same<T, bool>::value ? kFieldBool
       : sizeof(T) == 1 ? (std::is_signed<typename std::conditional<std::is_enum<T>::value,
                                                                     std::underlying_type<T>, std::common_type<T>>::type::type>::value
                               ? kFieldI8 : kFieldU8)
       : kFieldI32;
}

// Calls f(path, pointer, kind) for each field.
template <class F> void forEachField(Patch& p, F&& f) {
  char n[64];
  auto at = [&](auto& v, const char* fmt, auto... a) {
    snprintf(n, sizeof(n), fmt, a...);
    f((const char*)n, (void*)&v, fieldKind<typename std::remove_reference<decltype(v)>::type>());
  };
  auto env = [&](EnvPatch& e, const char* pre) {
    at(e.type, "%s.type", pre); at(e.startLevel, "%s.startLevel", pre); at(e.releaseLevel, "%s.releaseLevel", pre);
    at(e.attack, "%s.attack", pre); at(e.decay, "%s.decay", pre); at(e.sustain, "%s.sustain", pre);
    at(e.release, "%s.release", pre); at(e.ac, "%s.ac", pre); at(e.dc, "%s.dc", pre); at(e.rc, "%s.rc", pre);
    at(e.level, "%s.level", pre); at(e.vTim, "%s.vTim", pre); at(e.vLev, "%s.vLev", pre);
    at(e.kCtr, "%s.kCtr", pre); at(e.kTim, "%s.kTim", pre); at(e.kLev, "%s.kLev", pre);
  };
  char pre[32];
  for (int i = 0; i < 3; i++) {
    OscPatch& o = p.osc[i];
    at(o.enabled, "osc[%d].enabled", i); at(o.type, "osc[%d].type", i); at(o.wave, "osc[%d].wave", i);
    at(o.position, "osc[%d].position", i); at(o.ring, "osc[%d].ring", i); at(o.keyTrack, "osc[%d].keyTrack", i);
    at(o.phaseOffset, "osc[%d].phaseOffset", i); at(o.pulseWidth, "osc[%d].pulseWidth", i);
    at(o.transpose, "osc[%d].transpose", i); at(o.fine, "osc[%d].fine", i); at(o.level, "osc[%d].level", i);
    at(o.filterMix, "osc[%d].filterMix", i); at(o.noiseType, "osc[%d].noiseType", i);
    at(o.noiseCut, "osc[%d].noiseCut", i); at(o.noiseQ, "osc[%d].noiseQ", i); at(o.noiseDeci, "osc[%d].noiseDeci", i);
    at(o.zoneCount, "osc[%d].zoneCount", i);
    at(o.nsMix, "osc[%d].nsMix", i); at(o.nsFalloff, "osc[%d].nsFalloff", i);
    at(o.nsDetune, "osc[%d].nsDetune", i); at(o.nsSpread, "osc[%d].nsSpread", i);
    at(o.pdWave, "osc[%d].pdWave", i); at(o.pdAmount, "osc[%d].pdAmount", i);
    for (int k = 0; k < 3; k++) {
      at(o.fm.wave[k], "osc[%d].fm.wave[%d]", i, k); at(o.fm.ratio[k], "osc[%d].fm.ratio[%d]", i, k);
      at(o.fm.freqOffset[k], "osc[%d].fm.freqOffset[%d]", i, k);
    }
    for (int c = 0; c < kFmConns; c++) {
      at(o.fm.amount[c], "osc[%d].fm.amount[%d]", i, c);
      snprintf(pre, sizeof(pre), "osc[%d].fm.env[%d]", i, c);
      env(o.fm.env[c], pre);
    }
  }
  at(p.velSplit, "velSplit"); at(p.split2, "split2");
  for (int k = 0; k < 2; k++) {
    at(p.velLayer[k], "velLayer[%d]", k); at(p.split3[k], "split3[%d]", k); at(p.velXFade[k], "velXFade[%d]", k);
  }
  at(p.cutoff, "cutoff"); at(p.resonance, "resonance"); at(p.keyTrack, "keyTrack"); at(p.filterEnvAmt, "filterEnvAmt");
  at(p.filterType, "filterType"); at(p.filterGain, "filterGain"); at(p.shelfGain, "shelfGain");
  at(p.filterPan, "filterPan"); at(p.drive, "drive"); at(p.vowel, "vowel"); at(p.combMix, "combMix"); at(p.route, "route");
  at(p.osc1Bypass, "osc1Bypass");
  at(p.f2.enabled, "f2.enabled"); at(p.f2.type, "f2.type"); at(p.f2.cutoff, "f2.cutoff");
  at(p.f2.resonance, "f2.resonance"); at(p.f2.keyTrack, "f2.keyTrack"); at(p.f2.gain, "f2.gain");
  at(p.f2.shelfGain, "f2.shelfGain"); at(p.f2.pan, "f2.pan"); at(p.f2.envAmt, "f2.envAmt");
  at(p.f2.drive, "f2.drive"); at(p.f2.vowel, "f2.vowel"); at(p.f2.combMix, "f2.combMix");
  env(p.ampEnv, "ampEnv"); env(p.filtEnv, "filtEnv"); env(p.pitchEnv, "pitchEnv"); env(p.env4, "env4"); env(p.env5, "env5");
  for (int i = 0; i < kLfos; i++) {
    LfoPatch& l = p.lfo[i];
    at(l.rate, "lfo[%d].rate", i); at(l.beats, "lfo[%d].beats", i); at(l.ampl, "lfo[%d].ampl", i); at(l.type, "lfo[%d].type", i);
    at(l.out, "lfo[%d].out", i); at(l.sync, "lfo[%d].sync", i); at(l.phaseOffset, "lfo[%d].phaseOffset", i);
    at(l.attack, "lfo[%d].attack", i); at(l.curve, "lfo[%d].curve", i); at(l.warp, "lfo[%d].warp", i);
    at(l.quantize, "lfo[%d].quantize", i); at(l.loopPattern, "lfo[%d].loopPattern", i);
    at(l.loopLength, "lfo[%d].loopLength", i);
  }
  for (int k = 0; k < kModSlots; k++) {
    ModSlot& m = p.mod[k];
    at(m.src, "mod[%d].src", k); at(m.scale, "mod[%d].scale", k); at(m.dest, "mod[%d].dest", k);
    at(m.index, "mod[%d].index", k); at(m.depth, "mod[%d].depth", k);
  }
  at(p.comb.enabled, "comb.enabled"); at(p.comb.negative, "comb.negative"); at(p.comb.cutoff, "comb.cutoff");
  at(p.comb.q, "comb.q"); at(p.comb.mix, "comb.mix");
  at(p.f3.enabled, "f3.enabled"); at(p.f3.type, "f3.type"); at(p.f3.cutoff, "f3.cutoff"); at(p.f3.q, "f3.q");
  at(p.f3.gain, "f3.gain"); at(p.f3.shelfGain, "f3.shelfGain");
  at(p.f3.drive, "f3.drive"); at(p.f3.vowel, "f3.vowel");
  at(p.unison, "unison"); at(p.unisonDetune, "unisonDetune"); at(p.unisonStereo, "unisonStereo");
  for (int k = 0; k < kMacros; k++) at(p.macro[k], "macro[%d]", k);
  at(p.glide, "glide"); at(p.mono, "mono"); at(p.reverbMix, "reverbMix"); at(p.reverbDecay, "reverbDecay");
  at(p.reverbRoom, "reverbRoom"); at(p.reverbDamp, "reverbDamp"); at(p.reverbGain, "reverbGain");
  at(p.reverbHpHz, "reverbHpHz"); at(p.reverbLpHz, "reverbLpHz"); at(p.reverbPre, "reverbPre"); at(p.reverbPreMix, "reverbPreMix");
  at(p.reverbWidth, "reverbWidth");
  at(p.volume, "volume");
  at(p.phaser.enabled, "phaser.enabled"); at(p.phaser.stages, "phaser.stages"); at(p.phaser.rate, "phaser.rate");
  at(p.phaser.depth, "phaser.depth"); at(p.phaser.feedback, "phaser.feedback"); at(p.phaser.width, "phaser.width");
  at(p.phaser.mix, "phaser.mix");
  at(p.chorus.enabled, "chorus.enabled"); at(p.chorus.type, "chorus.type"); at(p.chorus.rate, "chorus.rate");
  at(p.chorus.depth, "chorus.depth"); at(p.chorus.delay, "chorus.delay"); at(p.chorus.feedback, "chorus.feedback");
  at(p.chorus.mix, "chorus.mix");
  at(p.delay.enabled, "delay.enabled"); at(p.delay.type, "delay.type"); at(p.delay.ratio, "delay.ratio");
  at(p.delay.time, "delay.time"); at(p.delay.beats, "delay.beats"); at(p.delay.feedback, "delay.feedback"); at(p.delay.eq, "delay.eq");
  at(p.delay.mix, "delay.mix");
}

// ---------------------------------------------------------------------------
// Envelope: ADSR segments with the Obsidian rational curve
// y = y0 + (y1 - y0) * t / (t + (k - 1)(1 - t)), evaluated once per block.
// AD decays to 0 and ignores the note-off. Bipolar attacks from SLv to 0, holds 0 (D and S unused)
// and releases to RLv [I]: the default pitch envelope of 352 presets (SLv 0, S 1) must be neutral.
// ---------------------------------------------------------------------------
struct Envelope {
  enum Stage { kIdle, kAttack, kDecay, kSustain, kRelease };
  Stage stage = kIdle;
  float t = 0, y0 = 0, y1 = 0, value = 0;

  void noteOn(const EnvPatch& p) {
    stage = kAttack; t = 0;
    y0 = p.type == kEnvBipolar ? p.startLevel : value;
    y1 = p.type == kEnvBipolar ? 0.0f : 1.0f;
  }
  void noteOff(const EnvPatch& p) {
    if (stage == kIdle || p.type == kEnvAD) return;
    stage = kRelease; t = 0; y0 = value;
    y1 = p.type == kEnvBipolar ? p.releaseLevel : 0.0f;
  }

  static inline float shape(float t, float k) { return t / (t + (k - 1.0f) * (1.0f - t)); }

  // Advances one block and returns the value at the end of it.
  float advance(const EnvPatch& p, float timeScale) {
    const float dt = (float)kBlock / kSampleRate;
    auto step = [&](float seconds, float k) {
      seconds *= timeScale;
      t += seconds > 0.0005f ? dt / seconds : 1.0f;
      if (t > 1.0f) t = 1.0f;
      value = y0 + (y1 - y0) * shape(t, k);
    };
    switch (stage) {
      case kIdle: value = p.type == kEnvBipolar ? p.releaseLevel : 0.0f; break;
      case kAttack:
        step(p.attack, p.ac);
        if (t >= 1.0f && p.type == kEnvBipolar) { stage = kSustain; value = 0.0f; }
        else if (t >= 1.0f) { stage = kDecay; t = 0; y0 = 1.0f; y1 = p.type == kEnvAD ? 0.0f : p.sustain; }
        break;
      case kDecay:
        step(p.decay, p.dc);
        if (t >= 1.0f) {
          if (p.type == kEnvAD) { stage = kIdle; value = 0; }
          else { stage = kSustain; value = p.sustain; }
        }
        break;
      case kSustain: value = p.type == kEnvBipolar ? 0.0f : p.sustain; break;
      case kRelease:
        step(p.release, p.rc);
        if (t >= 1.0f) { stage = kIdle; value = y1; }
        break;
    }
    return value;
  }
};

// ---------------------------------------------------------------------------
// 4-pole multimode ladder, decompiled from NS2 (setup 0x10054b098, render 0x100556498 and
// family; see 04-filters.md). Drive sets the saturator around the feedback point.
// ---------------------------------------------------------------------------
// Each stage is y = (1 - G) y + G (0.769 s + 0.231 s_prev). The stage input is
//   s = sat1 * shape(clamp(sat0 * (cx * x - k * y4), -1, 1)),  shape(v) = v - v^3 / 3
// with sat0 = 4.9 d^3 + 0.1 and sat1 = (14.5 d^3 + 1.5) / sat0, d = Drive. The output mixes
// s and the stage taps per mode; G, k and cx ramp linearly over the block, like NS2.
struct Ladder {
  float s1 = 0, y1 = 0, y2 = 0, y3 = 0, y4 = 0;
  float G = 0.1f, k = 0, cx = 1, gStep = 0, kStep = 0, cxStep = 0;
  float sat0 = 0.1f, sat1 = 15.0f, c[5] = {};  // output taps: s, y1..y4
  FilterType mode = kFilterLP24A;
  bool primed = false;

  static bool lowPass(FilterType t) { return t == kFilterLP24A || t == kFilterLP12A; }
  static bool notch(FilterType t) { return t == kFilterNotch6A || t == kFilterNotch12A; }

  void set(float hz, float q, float drive, FilterType type) {
    mode = type;
    const float fn = fminf(fmaxf(hz / kSampleRate, 0.0f), 0.25f), f = fn * kSampleRate;
    const float d = fminf(fmaxf(drive, 0.0f), 1.0f), d3 = d * d * d;
    sat0 = 4.9f * d3 + 0.1f;
    sat1 = (14.5f * d3 + 1.5f) / sat0;
    if (f < 40.0f) q *= 0.5f + 0.0125f * f;
    switch (type) {
      case kFilterBP6A: case kFilterBP12A: q *= 0.82f - 0.22f * d; break;
      case kFilterLP12A: q *= (sat0 > 1.0f ? 1.0f / sat0 : 1.0f) * (0.8f + 0.2f * d); break;
      case kFilterLP24A: if (sat0 > 1.0f) q /= sat0; break;
      case kFilterHP12A: q *= (sat0 > 1.0f ? 1.0f / sat0 : 1.0f) * 0.8f; break;
      case kFilterHP24A: q *= (sat0 > 1.0f ? 1.0f / sat0 : 1.0f) * (0.83f - 0.33f * d); break;
      default: break;
    }
    if (q >= 0.5f) q = 0.5f + 0.5f * sqrtf(2.0f * q - 1.0f);
    const float w = 6.28318531f * fn, w2 = w * w;
    const float cf = fn * (0.33049556f + 0.13697344f * w2 - 3.6557019f * fn);
    const float gT = fn * (6.2153273f + 0.8677079f * w2) + w * fn * (-2.7281590f - 0.12692034f * w2);
    // NS2 caps k at 3 / (sat0 * sat1) unless a flag of the filter is set [I]: without the cap, Drive
    // pushes the loop gain past self-oscillation even at moderate Q.
    const float kT = notch(type) ? q : fminf(q * (3.0087f + 3.0f * cf), 3.0f / (sat0 * sat1));
    const float* tap;
    static const float kLp12[5] = {0, 0, 0.75f, 0, 0}, kLp24[5] = {0, 0, 0, 0, 0.75f};
    static const float kBp6[5] = {0, 1.5f, -1.5f, 0, 0}, kBp12[5] = {0, 0, 3.0f, -6.0f, 3.0f};
    static const float kHp12[5] = {0.7f, -1.4f, 0.7f, 0, 0}, kHp24[5] = {0.7f, -2.8f, 4.2f, -2.8f, 0.7f};
    static const float kN6[5] = {0.7f, -1.4f, 1.4f, 0, 0}, kN12[5] = {0.7f, -2.8f, 5.6f, -5.6f, 2.8f};
    switch (type) {
      case kFilterLP12A: tap = kLp12; break;
      case kFilterBP6A: tap = kBp6; break;
      case kFilterBP12A: tap = kBp12; break;
      case kFilterHP12A: tap = kHp12; break;
      case kFilterHP24A: tap = kHp24; break;
      case kFilterNotch6A: tap = kN6; break;
      case kFilterNotch12A: tap = kN12; break;
      default: tap = kLp24; break;
    }
    // LP modes scale the input by 1 + 0.8k (bass compensation); the others mix s at tap[0].
    const float cxT = lowPass(type) ? 1.0f + 0.8f * kT : tap[0];
    for (int i = 0; i < 5; i++) c[i] = tap[i];
    if (!primed) { G = gT; k = kT; cx = cxT; primed = true; }
    gStep = (gT - G) * (1.0f / kBlock);
    kStep = (kT - k) * (1.0f / kBlock);
    cxStep = (cxT - cx) * (1.0f / kBlock);
  }

  inline float stage(float u) {
    float v = u * sat0;
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return sat1 * (v - v * v * v * (1.0f / 3.0f));
  }

  inline float tick(float x) {
    G += gStep;
    k += kStep;
    cx += cxStep;
    const bool lp = lowPass(mode), nt = notch(mode);
    const float s = stage(lp ? cx * x - k * y4 : (nt ? x : x - k * y4));
    const float a = 0.76923078f * G, b = 0.23076925f * G, r = 1.0f - G;
    const float n1 = a * s + b * s1 + r * y1;
    const float n2 = a * n1 + b * y1 + r * y2;
    const float n3 = a * n2 + b * y2 + r * y3;
    const float n4 = a * n3 + b * y3 + r * y4;
    s1 = s; y1 = n1; y2 = n2; y3 = n3; y4 = n4;
    if (lp) return c[2] * y2 + c[4] * y4;
    const float taps = c[1] * y1 + c[2] * y2 + c[3] * y3 + c[4] * y4;
    return cx * s + (nt ? k * taps : taps);
  }
};

struct Osc {
  uint32_t phase = 0, inc = 0;
  int mip = 0;
  inline float tick(const int16_t* tab) {
    uint32_t idx = phase >> 22;
    float frac = (float)((phase >> 6) & 0xffff) * (1.0f / 65536.0f);
    float a = tab[idx], b = tab[idx + 1];
    phase += inc;
    return (a + (b - a) * frac) * (1.0f / 32768.0f);
  }
  // A pulse of width `width` (cycles) as the difference of two band-limited saw reads.
  // Bi-Pulse: a positive pulse of the width at the start of the cycle and a negative one half a
  // cycle later, zero in between (the shape of NS2's renders): the pulse minus itself shifted.
  inline float tickBiPulse(const int16_t* saw, uint32_t width) {
    auto at = [&](uint32_t p) {
      const float fr = (float)((p >> 6) & 0xffff) * (1.0f / 65536.0f);
      const uint32_t i = p >> 22;
      return saw[i] + (saw[i + 1] - saw[i]) * fr;
    };
    const uint32_t p1 = phase + 0x80000000u;
    const float a = at(p1 - width) - at(p1), b = at(phase - width) - at(phase);
    phase += inc;
    return (a - b) * (1.0f / 32768.0f);
  }
  // Starts on the narrow positive part, like NS2: reads the rising saw half a cycle ahead.
  inline float tickPulse(const int16_t* saw, uint32_t width) {
    const uint32_t p1 = phase + 0x80000000u, p2 = p1 - width;
    float frac = (float)((p1 >> 6) & 0xffff) * (1.0f / 65536.0f);
    float frac2 = (float)((p2 >> 6) & 0xffff) * (1.0f / 65536.0f);
    const uint32_t i1 = p1 >> 22, i2 = p2 >> 22;
    float a = saw[i1] + (saw[i1 + 1] - saw[i1]) * frac;
    float b = saw[i2] + (saw[i2 + 1] - saw[i2]) * frac2;
    phase += inc;
    return (b - a) * (1.0f / 32768.0f);
  }
  // Linear crossfade between two frames of a wavetable (Mode=Crossfade).
  inline float tick2(const int16_t* tabA, const int16_t* tabB, float xf) {
    uint32_t idx = phase >> 22;
    float frac = (float)((phase >> 6) & 0xffff) * (1.0f / 65536.0f);
    float a = tabA[idx] + (tabA[idx + 1] - tabA[idx]) * frac;
    float b = tabB[idx] + (tabB[idx + 1] - tabB[idx]) * frac;
    phase += inc;
    return a + (b - a) * xf;
  }
};

// RBJ biquad (the "-D" family designers), transposed direct form II.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  float d[5] = {};  // per-sample coefficient steps towards the last set()
  bool primed = false;

  // Below ~20 Hz the poles sit so close to 1 that float rounding and the per-sample coefficient
  // ramp can make the filter diverge (a random LFO on a 30 Hz LP 12-D did), so f is kept above.
  void set(NoiseType type, float f, float q) {
    f = f < 20.0f ? 20.0f : (f > 0.45f * kSampleRate ? 0.45f * kSampleRate : f);
    float w = 6.28318531f * f / kSampleRate, cs = cosf(w), alpha = sinf(w) / (2.0f * q);
    float nb0, nb1, nb2;
    switch (type) {
      case kNoiseHighPass: nb0 = 0.5f * (1.0f + cs); nb1 = -(1.0f + cs); nb2 = nb0; break;
      case kNoiseBandPass: nb0 = alpha; nb1 = 0.0f; nb2 = -alpha; break;
      case kNoiseNotch: nb0 = 1.0f; nb1 = -2.0f * cs; nb2 = 1.0f; break;
      default: nb0 = 0.5f * (1.0f - cs); nb1 = 1.0f - cs; nb2 = nb0; break;
    }
    const float inv = 1.0f / (1.0f + alpha);
    const float t[5] = {nb0 * inv, nb1 * inv, nb2 * inv, -2.0f * cs * inv, (1.0f - alpha) * inv};
    target(t);
  }

  // RBJ peaking EQ, or low shelf with slope 1 (which shelf NS2's EQ Shelf is, is [?]).
  // EQ Peak takes the designer's Q; EQ Shelf takes the raw Q (0..1). NS2's shelf (0x10053aa74) is an RBJ
  // low shelf with slope S = clamp(Q^2, 0.01, 1) whose output is scaled by 1/A: a tilt of +SGan/2 dB below
  // the corner and -SGan/2 above it (matches NS2 renders at +-12 and +24 dB, Q 0 and 1).
  void setEq(bool peak, float f, float q, float gainDb) {
    f = f < 20.0f ? 20.0f : (f > 0.45f * kSampleRate ? 0.45f * kSampleRate : f);
    const float A = exp2f(gainDb * (3.32193f / 40.0f));  // 10^(dB/40)
    const float w = 6.28318531f * f / kSampleRate, cs = cosf(w), sn = sinf(w);
    float t[5];
    if (peak) {
      const float alpha = sn / (2.0f * q), inv = 1.0f / (1.0f + alpha / A);
      t[0] = (1.0f + alpha * A) * inv; t[1] = -2.0f * cs * inv; t[2] = (1.0f - alpha * A) * inv;
      t[3] = -2.0f * cs * inv; t[4] = (1.0f - alpha / A) * inv;
    } else {
      const float S = fminf(fmaxf(q * q, 0.01f), 1.0f);
      const float sq = sn * sqrtf(A * (2.0f + (A + 1.0f / A) * (1.0f / S - 1.0f)));
      const float inv = 1.0f / ((A + 1.0f) + (A - 1.0f) * cs + sq);
      t[0] = ((A + 1.0f) - (A - 1.0f) * cs + sq) * inv;  // A * (...) / A: the 1/A tilt gain
      t[1] = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs) * inv;
      t[2] = ((A + 1.0f) - (A - 1.0f) * cs - sq) * inv;
      t[3] = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs) * inv;
      t[4] = ((A + 1.0f) + (A - 1.0f) * cs - sq) * inv;
    }
    target(t);
  }

  // Ramps the coefficients towards t over the next block (jumps there on the first call).
  void target(const float t[5]) {
    float* c[5] = {&b0, &b1, &b2, &a1, &a2};
    for (int i = 0; i < 5; i++) {
      if (!primed) *c[i] = t[i];
      d[i] = (t[i] - *c[i]) * (1.0f / kBlock);
    }
    primed = true;
  }

  inline float tick(float x) {
    float o = b0 * x + z1;
    z1 = b1 * x - a1 * o + z2;
    z2 = b2 * x - a2 * o;
    b0 += d[0]; b1 += d[1]; b2 += d[2]; a1 += d[3]; a2 += d[4];
    return o;
  }

  // One block in place, with the state and coefficients kept in registers.
  void run(float* x) {
    float c0 = b0, c1 = b1, c2 = b2, c3 = a1, c4 = a2, s1 = z1, s2 = z2;
    const float e0 = d[0], e1 = d[1], e2 = d[2], e3 = d[3], e4 = d[4];
    for (int n = 0; n < kBlock; n++) {
      float in = x[n], o = c0 * in + s1;
      s1 = c1 * in - c3 * o + s2;
      s2 = c2 * in - c4 * o;
      x[n] = o;
      c0 += e0; c1 += e1; c2 += e2; c3 += e3; c4 += e4;
    }
    b0 = c0; b1 = c1; b2 = c2; a1 = c3; a2 = c4; z1 = s1; z2 = s2;
  }
};

// Noise oscillator (03-oscillators.md): additive integer generator, note-following sample & hold,
// then an RBJ biquad for the filtered types. Coefficients are updated once per block.
struct Noise {
  uint32_t x = 0x2545f491, y = 0x9e3779b9;
  float hold = 0, phase = 1.0f, rate = 1.0f;
  Biquad filter;
  bool filtered = false;

  static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

  void set(const OscPatch& p, float pitch, float cutMod, float qMod, float deciMod) {
    rate = exp2f((pitch - 60.0f - 128.0f * clamp01(p.noiseDeci + deciMod)) / 12.0f);
    if (rate > 1.0f) rate = 1.0f;
    filtered = p.noiseType != kNoiseWhite;
    if (!filtered) return;
    float f = 20.0f * powf(1000.0f, clamp01(p.noiseCut + cutMod));
    float qn = clamp01(p.noiseQ + qMod);
    float q = 0.25f + (p.noiseType == kNoiseNotch ? 3.75f : 15.75f) * qn * qn;
    if (f < 40.0f) q *= 0.025f * f;
    filter.set(p.noiseType, f, q);
  }

  inline float tick() {
    phase += rate;
    if (phase >= 1.0f) {
      phase -= 1.0f;
      uint32_t nx = (x >> 2) + 4 * x + y;
      y = nx + y;
      x = nx;
      hold = (float)(int32_t)nx * (1.0f / 2147483648.0f);
    }
    return filtered ? filter.tick(hold) : hold;
  }
};

// A filter slot (F1 or F3) of any type the engine has: the "-A" types run on the ladder,
// the "-D" and EQ types on the biquad (04-filters.md).
// Formant-M/F: 4 parallel band-passes on F0..F3 of Peterson & Barney's vowels (04-filters.md),
// FVwl morphing through the 10 vowels. Fitted to NS2 renders of white noise [P]: the frequencies
// scale with the cutoff as Hz/800 (CutO 0.5 = 600 Hz plays them at 0.75x) and stop at 14.4 kHz,
// each band-pass has Q 0.94 * 4^Q and peak gain Q^1.5 (F0 at 0.7 of that).
struct Formant {
  float ic1[4] = {}, ic2[4] = {}, a1[4] = {}, a2[4] = {}, a3[4] = {}, k[4] = {}, g[4] = {};

  void set(bool female, float hz, float q, float vowel) {
    static const float kM[10][4] = {{136, 266, 2293, 2937}, {135, 392, 1993, 2568}, {128, 526, 1854, 2481},
                                    {125, 664, 1727, 2420}, {129, 631, 1192, 2377}, {124, 718, 1090, 2442},
                                    {127, 568, 836, 2403}, {136, 437, 1023, 2245}, {139, 307, 876, 2239},
                                    {132, 489, 1360, 1709}};
    static const float kF[10][4] = {{231, 310, 2783, 3312}, {232, 441, 2474, 3063}, {223, 610, 2330, 2990},
                                    {210, 860, 2050, 2850}, {212, 760, 1400, 2780}, {212, 850, 1220, 2810},
                                    {216, 590, 920, 2710}, {232, 470, 1160, 2680}, {231, 370, 950, 2670},
                                    {218, 500, 1640, 1960}};
    const float (*t)[4] = female ? kF : kM;
    const float x = Noise::clamp01(vowel) * 9.0f;
    const int i = x >= 9.0f ? 8 : (int)x;
    const float fr = x - (float)i, scale = hz / 800.0f, qbp = 0.94f * powf(4.0f, Noise::clamp01(q));
    for (int j = 0; j < 4; j++) {
      float f = (t[i][j] + (t[i + 1][j] - t[i][j]) * fr) * scale;
      f = f < 20.0f ? 20.0f : (f > 14400.0f ? 14400.0f : f);
      const float w = tanf(3.14159265f * f / kSampleRate);
      k[j] = 1.0f / qbp;
      a1[j] = 1.0f / (1.0f + w * (w + k[j]));
      a2[j] = w * a1[j];
      a3[j] = w * a2[j];
      g[j] = (j == 0 ? 0.7f : 1.0f) * sqrtf(qbp);  // the band output peaks at qbp
    }
  }

  // Topology-preserving state-variable band-passes, summed.
  void run(float* x, float inGain) {
    for (int n = 0; n < kBlock; n++) {
      const float in = x[n] * inGain;
      float y = 0.0f;
      for (int j = 0; j < 4; j++) {
        const float v3 = in - ic2[j], v1 = a1[j] * ic1[j] + a2[j] * v3, v2 = ic2[j] + a2[j] * ic1[j] + a3[j] * v3;
        ic1[j] = 2.0f * v1 - ic1[j];
        ic2[j] = 2.0f * v2 - ic2[j];
        y += g[j] * v1;
      }
      x[n] = y;
    }
  }
};

struct Filter {
  Ladder ladder;
  Biquad bq;
  Formant formant;
  FilterType type = kFilterLP24A;
  float wsScale = 0, wsOut = 1, wsMix = 1;  // waveshaper input (table index per unit), output gain, wet share

  static bool onLadder(FilterType t) {
    return t == kFilterLP24A || t == kFilterLP12A || t == kFilterHP12A || t == kFilterHP24A || (t >= kFilterBP6A && t <= kFilterNotch12A);
  }
  static bool shaper(FilterType t) { return t >= kFilterWSTube && t <= kFilterWSTri; }
  static bool comb(FilterType t) { return t == kFilterCombPos || t == kFilterCombNeg; }
  static constexpr float kWsIn = 2311.0f, kWsOut = 0.61f;  // table index per unit of d; output

  // `drive` (Drv) sets the ladder's saturator and the waveshapers' input gain. NS2's WaveShape setup
  // (0x1000fd398) scales the input by d and the output by 1 - 0.3d (x1.5 for Tube), with
  // d = a + (1 - a) * Drive, a = 0.15 for Tube and Enhance, 0.02 for the others. The scale
  // factors are fitted to NS2 renders (04-filters.md) [P]: WSGn changes nothing there. A
  // waveshaper mixes dry and shaped signal by its normalized cutoff `cut` [P].
  void set(FilterType t, float hz, float q, float gainDb, float drive = 0.0f, float cut = 1.0f, float vowel = 0.5f) {
    type = t;
    if (t == kFilterFormantM || t == kFilterFormantF) { formant.set(t == kFilterFormantF, hz, q, vowel); return; }
    if (shaper(t)) {
      const int s = t - kFilterWSTube;
      const float a = (s == 0 || s == 3) ? 0.15f : 0.02f, d = a + (1.0f - a) * Noise::clamp01(drive);
      wsScale = kWsIn * d;
      wsMix = Noise::clamp01(cut);
      wsOut = kWsOut * (1.0f - 0.3f * d) * (s == 0 ? 1.5f : 1.0f);
      return;
    }
    if (t == kFilterOff || comb(t)) return;
    if (onLadder(t)) { ladder.set(hz, q, drive, t); return; }
    if (t == kFilterEQPeak || t == kFilterEQShelf) { bq.setEq(t == kFilterEQPeak, hz, t == kFilterEQPeak ? 0.25f + 7.75f * q * q : q, gainDb); return; }
    const float qq = q * (hz < 40.0f ? hz / 40.0f : 1.0f);
    bq.set(t == kFilterHP12D ? kNoiseHighPass : kNoiseLowPass, hz, 0.5f + 5.5f * qq * qq);
  }

  // One block in place, with the input scaled by `inGain` first.
  void run(float* x, float inGain) {
    if (shaper(type)) {
      // Output Gain scales the curve's output; the voice's half level (inGain at OGn 1) its input.
      const float* t = gBank->shaper[type - kFilterWSTube];
      const float sc = 0.5f * wsScale, out = wsOut * 2.0f * inGain * wsMix, dry = inGain * (1.0f - wsMix);
      for (int n = 0; n < kBlock; n++) {
        float i = 512.0f + x[n] * sc;
        i = i < 0.0f ? 0.0f : (i > 1023.999f ? 1023.999f : i);
        const int k = (int)i;
        x[n] = (t[k] + (t[k + 1] - t[k]) * (i - (float)k)) * out + x[n] * dry;
      }
      return;
    }
    if (type == kFilterFormantM || type == kFilterFormantF) {
      formant.run(x, inGain);
    } else if (type == kFilterOff) {
      for (int n = 0; n < kBlock; n++) x[n] *= inGain;
    } else if (onLadder(type)) {
      // The voice feeds its filters half level; NS2's ladder saturates as if fed full level, so it
      // gets 2x and its output is halved back (fitted against NS2 renders at Drive 0.3 to 1).
      for (int n = 0; n < kBlock; n++) x[n] = 0.5f * ladder.tick(2.0f * inGain * x[n]);
    } else {
      for (int n = 0; n < kBlock; n++) x[n] *= inGain;
      bq.run(x);
    }
  }
};

// Per-oscillator velocity gains at note-on, decompiled from cOscTriple (03-oscillators.md):
// an oscillator with gain 0 does not play. A crossfade narrower than 0.01 is a hard switch.
static void velocityGains(const Patch& p, float v, float g[3]) {
  const float kMinFade = 0.01f;
  switch (p.velSplit) {
    case kVelLayer:
      g[0] = 1.0f;
      for (int k = 0; k < 2; k++) {
        const float lvl = p.velLayer[k], x = p.velXFade[k], from = lvl - x * lvl;
        g[k + 1] = v >= lvl ? 1.0f : (x >= kMinFade && v > from ? (v - from) / (lvl - from) : 0.0f);
      }
      break;
    case kVelSplit2: {
      const float lvl = p.split2, x = p.velXFade[0], w = x * (1.0f - lvl);
      g[2] = 1.0f;
      if (v <= lvl) { g[0] = 1.0f; g[1] = 0.0f; }
      else if (x >= kMinFade && v < lvl + w) { g[0] = (lvl + w - v) / w; g[1] = 1.0f - g[0]; }
      else { g[0] = 0.0f; g[1] = 1.0f; }
      break;
    }
    case kVelSplit3: {
      const float l1 = p.split3[0], l2 = p.split3[1];
      if (v <= l1) {
        g[0] = 1.0f; g[1] = 0.0f; g[2] = 0.0f;
      } else if (v <= l2) {
        const float x = p.velXFade[0], w = (l2 - l1) * x;
        g[2] = 0.0f;
        if (x >= kMinFade && v < l1 + w) { g[0] = (l1 + w - v) / w; g[1] = 1.0f - g[0]; }
        else { g[0] = 0.0f; g[1] = 1.0f; }
      } else {
        const float x = p.velXFade[1], w = x * (1.0f - l2);
        g[0] = 0.0f;
        if (x >= kMinFade && v < l2 + w) { g[2] = (v - l2) / w; g[1] = 1.0f - g[2]; }
        else { g[1] = 0.0f; g[2] = 1.0f; }
      }
      break;
    }
  }
}

// Sample oscillator: one zone picked at note-on, linear interpolation, forward smpl loop.
struct SamplePlayer {
  const SampleZone* zone = nullptr;
  uint64_t phase = 0;  // 32.32 position in the sample
  bool done = true;

  void start(const OscPatch& p, int note) {
    zone = nullptr;
    for (int z = 0; z < p.zoneCount && !zone; z++)
      if (note >= p.zones[z].lo && note <= p.zones[z].hi && p.zones[z].sample) zone = &p.zones[z];
    phase = 0;
    done = zone == nullptr || zone->sample->length < 2;
  }

  void render(float pitch, float* out) {
    // A newer preset may have rebound the zone pool under a sounding voice.
    if (done || !zone->sample) { memset(out, 0, sizeof(float) * kBlock); return; }
    const Sample& s = *zone->sample;
    const bool loop = zone->loop && s.loopEnd > s.loopStart;
    const uint32_t end = loop ? s.loopEnd : s.length - 1;
    const uint64_t loopLen = (uint64_t)(s.loopEnd - s.loopStart) << 32;
    const float ratio = exp2f((pitch + zone->tune - (float)zone->root) / 12.0f) * s.rate;
    const uint64_t inc = (uint64_t)(ratio * 4294967296.0f);
    const float gain = zone->vol * (1.0f / 32768.0f);
    for (int n = 0; n < kBlock; n++) {
      uint32_t i = (uint32_t)(phase >> 32);
      if (i >= end) {
        if (!loop) { done = true; memset(out + n, 0, sizeof(float) * (kBlock - n)); return; }
        while (i >= end) { phase -= loopLen; i = (uint32_t)(phase >> 32); }
      }
      const uint32_t j = loop && i + 1 == end ? s.loopStart : i + 1;
      const float frac = (float)((uint32_t)phase >> 8) * (1.0f / 16777216.0f);
      const float a = s.data[i], b = s.data[j];
      out[n] = (a + (b - a) * frac) * gain;
      phase += inc;
    }
  }
};

// ---------------------------------------------------------------------------
// LFO (cLFOSimple, 05-envelopes-lfos.md): shape -> OutT -> Warp -> Qtz -> Ampl -> Atk fade-in.
// Square and Random skip the warp. Saw Double and the Random Loop patterns are [?].
// ---------------------------------------------------------------------------
struct Lfo {
  float phase = 0, time = 0;
  uint32_t rnd = 1, seed = 1;
  float held = 0;
  int step = 0;

  static float uniform(float v) { return (float)(esp_random() >> 8) * (1.0f / 16777216.0f) * v; }

  void nextRandom() {
    rnd = (rnd * 31415821u + 1u) & 0x3FFFFFFFu;
    held = (float)rnd * (2.0f / 1073741824.0f) - 1.0f;
  }

  // Note-on (or engine start): Sync=KEY restarts at PhOf, Sync=OFF takes a random phase and
  // Sync=GLOBAL the shared phase (the engine's free-running LFO stands in for the transport).
  void start(const LfoPatch& p, float sharedPhase) {
    time = 0;
    phase = p.sync == kLfoKey ? fminf(p.phaseOffset, 0.999f) : (p.sync == kLfoGlobal ? sharedPhase : uniform(0.999f));
    seed = 0x9E3779B1u * (uint32_t)p.loopPattern;
    rnd = p.type == kLfoRandomLoop ? seed : (esp_random() & 0x3FFFFFFFu);
    step = -1;
    if (p.type == kLfoRandomLoop) catchUp(p);
    else nextRandom();
  }

  // Random Loop plays its whole loop in one period, RLen steps of period / RLen each [I]: Arping
  // On (8/16 bar, RLen 8) retriggers on 1/16 notes in NS2.
  void catchUp(const LfoPatch& p) {
    const int n = p.loopLength > 1 ? p.loopLength : 1;
    const int s = (int)(phase * (float)n);
    while (step < s) { step++; nextRandom(); }
  }

  float shape(const LfoPatch& p) const {
    const float ph = phase;
    switch (p.type) {
      case kLfoSquare: return ph < 0.5f ? 1.0f : -1.0f;
      case kLfoTriangle: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
      case kLfoSawUp: return 2.0f * ph - 1.0f;
      case kLfoSawDown: return 1.0f - 2.0f * ph;
      case kLfoSawDouble: { float h = 2.0f * ph; return 2.0f * (h - floorf(h)) - 1.0f; }
      case kLfoRandom:
      case kLfoRandomLoop: return held;
      default: {
        const float c = p.curve < 0.02f ? 0.02f : (p.curve > 0.98f ? 0.98f : p.curve);
        const float b = ph < c ? 0.5f * ph / c : 0.5f + 0.5f * (ph - c) / (1.0f - c);
        // The sine mip of the wave bank, linearly interpolated (NS2 uses a 512-point table).
        const int16_t* t = gBank->table[kSine][0];
        const float x = b * kTableLen;
        int i = (int)x;
        if (i >= kTableLen) i = kTableLen - 1;
        return (t[i] + (t[i + 1] - t[i]) * (x - (float)i)) * (1.0f / 32767.0f);
      }
    }
  }

  // Output for this block, then advances the phase by one block.
  float tick(const LfoPatch& p, float rateMod, float levelMod) {
    const float dt = (float)kBlock / kSampleRate;
    float y = shape(p);
    if (p.out == kLfoUniHalf) y = y > 0.0f ? y : 0.0f;
    else if (p.out == kLfoUniFull) y = 0.5f * y + 0.5f;
    if (p.type != kLfoSquare && p.type != kLfoRandom && p.type != kLfoRandomLoop && p.warp != 0.5f) {
      float w = 0.03f + 0.94f * p.warp;
      w = w < 0.03f ? 0.03f : (w > 0.97f ? 0.97f : w);
      const float k = 1.0f / w, a = fabsf(y), t = a / (a + (k - 1.0f) * (1.0f - a));
      y = y < 0.0f ? -t : t;
    }
    if (p.quantize > 0 && p.type != kLfoRandomLoop) y = floorf(y * p.quantize + 0.5f) / (float)p.quantize;
    float amp = p.ampl + levelMod;
    amp = amp < 0.0f ? 0.0f : (amp > 1.0f ? 1.0f : amp);
    if (p.attack > 0.0f && time < p.attack) amp *= time / p.attack;
    float rate = (p.beats > 0.0f ? gBpm / (60.0f * p.beats) : p.rate) + 60.0f * rateMod * rateMod * rateMod;
    rate = rate < 0.0f ? 0.0f : (rate > 60.0f ? 60.0f : rate);
    phase += rate * dt;
    time += dt;
    if (p.type == kLfoRandomLoop) {
      if (phase >= 1.0f) { phase -= floorf(phase); rnd = seed; step = -1; }
      catchUp(p);
    } else if (phase >= 1.0f) {
      phase -= floorf(phase);
      nextRandom();
    }
    return y * amp;
  }
};

// The matrix sum per destination for one block.
struct Mods {
  float osc[4][kOscDests];  // [3] = Osc All
  float voice[kDstDrive2 - kDstCutoff + 1];  // kDstCutoff .. kDstDrive2
  float env[kEnvs][6];      // Attack, Decay, Sustain, Release, Level, Retrigger
  float lfo[kLfos][2];      // Rate, Level
  float global[5];          // Global Filter Cutoff, Q, Reverb Mix, Multi-FX Mix, Delay Mix
};

static void evalMatrix(const Patch& p, const float* src, Mods& m) {
  memset(&m, 0, sizeof(m));
  for (const ModSlot& s : p.mod) {
    if (s.src == kSrcNone) continue;
    float v = src[s.src] * s.depth;
    if (s.scale != kSrcNone) v *= src[s.scale];
    if (s.dest < kOscDests) m.osc[s.index][s.dest] += v;
    else if (s.dest <= kDstDrive2) m.voice[s.dest - kDstCutoff] += v;
    else if (s.dest <= kDstEnvRetrigger) m.env[s.index][s.dest - kDstEnvAttack] += v;
    else if (s.dest <= kDstLfoLevel) m.lfo[s.index][s.dest - kDstLfoRate] += v;
    else m.global[s.dest - kDstGlobalCutoff] += v;
  }
}

// Envelope parameters with modulation: times move in NS2's x = (T/Tmax)^(1/3) domain
// (05-envelopes-lfos.md), sustain and level add directly. Tmax of the release is [?].
static EnvPatch modEnv(const EnvPatch& e, const float* m) {
  if (m[0] == 0.0f && m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f && m[4] == 0.0f) return e;
  auto time = [](float t, float mod, float tmax) {
    if (mod == 0.0f) return t;
    float x = cbrtf(t / tmax) + mod;
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return tmax * x * x * x;
  };
  EnvPatch r = e;
  r.attack = time(e.attack, m[0], 10.0f);
  r.decay = time(e.decay, m[1], 20.0f);
  r.release = time(e.release, m[3], 20.0f);
  r.sustain = Noise::clamp01(e.sustain + m[2]);
  r.level = Noise::clamp01(e.level + m[4]);
  return r;
}

// FM oscillator, as decompiled from cOscTypeFM (03-oscillators.md): 32-bit integer phases,
// 256-point band-limited tables from NS2 (fm_waves.h), feedback averaged over two samples, and
// modulators read one sample late. A connection amount is
// clamp((1 - Levl + Levl*env) * velocity scale * (Amt + mod), 0, 1.785)^2 * K.
struct FmOsc {
  static constexpr float kK[kFmConns] = {272625.0f, 272625.0f, 272625.0f, 262140.0f, 262140.0f,
                                         16383.75f, 16383.75f, 16383.75f};
  Envelope env[kFmConns];
  EnvScale sc[kFmConns];
  uint32_t ph[3] = {};
  int32_t amt[kFmConns] = {};  // amounts reached at the end of the previous block
  int16_t o1 = 0, o2 = 0, avg1 = 0, avg2 = 0;
  bool primed = false;

  void start(const FmPatch& p, int note, float vel) {
    for (int c = 0; c < kFmConns; c++) {
      sc[c] = envScale(p.env[c], note, vel);
      env[c].noteOn(p.env[c]);
    }
    ph[0] = ph[1] = ph[2] = 0;
    o1 = o2 = avg1 = avg2 = 0;
    primed = false;
  }

  void release(const FmPatch& p) { for (int c = 0; c < kFmConns; c++) env[c].noteOff(p.env[c]); }

  static const int16_t* table(int wave, float hz44) {
    const FmBand* b = kFmBandsOf[wave];
    int i = 0;
    while (b[i].maxHz <= hz44) i++;
    return kFmTables[b[i].table];
  }

  // Phase modulation wraps modulo 2^32, as NS2's 32-bit multiply-adds do.
  static inline uint32_t pm(int32_t amount, int32_t x) { return (uint32_t)amount * (uint32_t)x; }

  static inline int16_t read(const int16_t* t, uint32_t phase) {
    const uint32_t i = phase >> 24;
    const int32_t frac = (int32_t)((phase >> 8) & 0xffff), a = t[i], b = t[i + 1];
    return (int16_t)(a + (((b - a) * frac) >> 16));
  }

  // `mod` points at the oscillator's kDstFmConn.. destinations of the matrix.
  void render(const FmPatch& p, float hz, const float* mod, float* out) {
    const float* ratioMod = mod + (kDstFmRatio - kDstFmConn);
    const int16_t* t[3];
    uint32_t inc[3];
    for (int k = 0; k < 3; k++) {
      float r = p.ratio[k] + 10.0f * ratioMod[k];
      r = r < 0.0f ? 0.0f : (r > 32.0f ? 32.0f : r);
      t[k] = table(p.wave[k], r * hz * (44100.0f / kSampleRate));
      inc[k] = (uint32_t)((r * hz + p.freqOffset[k]) * (4294967296.0f / kSampleRate));
    }
    int32_t target[kFmConns], step[kFmConns], cur[kFmConns];
    for (int c = 0; c < kFmConns; c++) {
      const float e = env[c].advance(p.env[c], sc[c].time), depth = 1.0f - p.env[c].level;
      float a = (1.0f - depth + depth * e) * sc[c].level * (p.amount[c] + mod[c]);
      a = a < 0.0f ? 0.0f : (a > 1.785f ? 1.785f : a);
      target[c] = (int32_t)(kK[c] * a * a);
      cur[c] = primed ? amt[c] : target[c];
      step[c] = (target[c] - cur[c]) / kBlock;
      amt[c] = target[c];
    }
    primed = true;
    // Like NS2's specialized renders: an operator runs only if it reaches the output this block.
    bool on[kFmConns];
    for (int c = 0; c < kFmConns; c++) on[c] = cur[c] != 0 || target[c] != 0;
    const bool need3 = on[7], need2 = on[6] || (on[4] && need3);
    const bool need1 = on[5] || (on[1] && need2) || (on[2] && need3);
    const int which = (need1 ? 1 : 0) | (need2 ? 2 : 0) | (need3 ? 4 : 0);
    switch (which) {
      case 1: run<true, false, false>(t, inc, cur, step, out); break;
      case 2: run<false, true, false>(t, inc, cur, step, out); break;
      case 3: run<true, true, false>(t, inc, cur, step, out); break;
      case 4: run<false, false, true>(t, inc, cur, step, out); break;
      case 5: run<true, false, true>(t, inc, cur, step, out); break;
      case 6: run<false, true, true>(t, inc, cur, step, out); break;
      case 7: run<true, true, true>(t, inc, cur, step, out); break;
      default:
        memset(out, 0, sizeof(float) * kBlock);
        for (int k = 0; k < 3; k++) ph[k] += inc[k] * kBlock;
        break;
    }
  }

  template <bool N1, bool N2, bool N3>
  void run(const int16_t* const* t, const uint32_t* inc, const int32_t* cur, const int32_t* step, float* out) {
    uint32_t p1 = ph[0], p2 = ph[1], p3 = ph[2];
    int32_t fb1 = cur[0], m12 = cur[1], m13 = cur[2], fb2 = cur[3], m23 = cur[4], l1 = cur[5], l2 = cur[6], l3 = cur[7];
    int32_t a1 = avg1, a2 = avg2, y1 = o1, y2 = o2;
    for (int n = 0; n < kBlock; n++) {
      float y = 0.0f;
      int16_t n1 = 0, n2 = 0;
      if (N1) {
        n1 = read(t[0], p1 + pm(fb1, a1));
        a1 = (int16_t)((n1 + a1) >> 1);
        y += (float)l1 * n1;
      }
      if (N2) {
        n2 = read(t[1], p2 + pm(fb2, a2) + pm(m12, y1));
        a2 = (int16_t)((n2 + a2) >> 1);
        y += (float)l2 * n2;
      }
      if (N3) y += (float)l3 * read(t[2], p3 + pm(m13, y1) + pm(m23, y2));
      y1 = n1;
      y2 = n2;
      out[n] = y * 2.4651e-9f;
      p1 += inc[0]; p2 += inc[1]; p3 += inc[2];
      if (N1) { fb1 += step[0]; l1 += step[5]; }
      if (N2) { m12 += step[1]; fb2 += step[3]; l2 += step[6]; }
      if (N3) { m13 += step[2]; m23 += step[4]; l3 += step[7]; }
    }
    ph[0] = p1; ph[1] = p2; ph[2] = p3;
    avg1 = (int16_t)a1; avg2 = (int16_t)a2; o1 = (int16_t)y1; o2 = (int16_t)y2;
  }
};
constexpr float FmOsc::kK[kFmConns];

// NanoSaw (cOscTypeNanoSaw, 03-oscillators.md): 7 saws with DPW antialiasing, the detune curve,
// spread and gains decompiled. Mono: the two 4-saw groups NS2 may pan with Widt are summed.
struct NanoSaw {
  uint32_t ph[7] = {};
  float prev[7] = {};

  void start() {
    ph[0] = 0x7FFFFFFFu;
    for (int k = 1; k < 7; k++) ph[k] = (uint32_t)esp_random();
    for (int k = 0; k < 7; k++) { const float p = (float)(int32_t)ph[k] * (1.0f / 2147483648.0f); prev[k] = p * p; }
  }

  void render(const OscPatch& o, float hz, float detuneMod, float mixMod, float* out) {
    const float d = fminf(fmaxf(o.nsDetune + detuneMod, 0.0f), 1.0f);
    const float D = 0.5f * (d < 0.6f ? d / 6.0f : 2.5f * d - 1.4f);
    const float sp = 2.25f - 2.0f * o.nsSpread * o.nsSpread;
    const float a3 = D, a2 = D * powf(1.5f, -sp), a1 = D * powf(3.0f, -sp);
    const float m = fminf(fmaxf(o.nsMix + mixMod, 0.0f), 1.0f), fo = o.nsFalloff;
    // The decompiled gains with two measured scales [P]: in NS2 renders the centre saw plays at 0.88
    // and the six others at 0.52 of them (Mix 0 / 0.5 / 1 match within 0.5 dB).
    const float S = 0.52f * 1.1f * (0.75f + 0.25f * fo) * m;
    const float ratio[7] = {1.0f, 1.0f - a1, 1.0f + a1, 1.0f - a2, 1.0f + a2, 1.0f - a3, 1.0f + a3};
    const float gain[7] = {0.88f * (1.0f - 0.5f * m), S * (1.0f - fo / 18.0f), S * (1.0f - fo / 18.0f), S * (1.0f - 2.0f * fo / 9.0f),
                           S * (1.0f - 2.0f * fo / 9.0f), S * (1.0f - 0.5f * fo), S * (1.0f - 0.5f * fo)};
    uint32_t inc[7];
    float k[7], naive[7];
    for (int j = 0; j < 7; j++) {
      const float f = fminf(fmaxf(hz * ratio[j], 0.0f), 0.45f * kSampleRate);
      inc[j] = (uint32_t)(f * (4294967296.0f / kSampleRate));
      // DPW: (p^2 - p^2[n-1]) * sr/4f is the saw. It divides by f, so below 40 Hz (no aliasing
      // to remove there) the saw is read directly.
      const bool dpw = f >= 40.0f;
      k[j] = dpw ? gain[j] * kSampleRate / (4.0f * f) : 0.0f;
      naive[j] = dpw ? 0.0f : gain[j];
    }
    for (int n = 0; n < kBlock; n++) {
      float y = 0.0f;
      for (int j = 0; j < 7; j++) {
        ph[j] += inc[j];
        const float p = (float)(int32_t)ph[j] * (1.0f / 2147483648.0f), x = p * p;
        y += (x - prev[j]) * k[j] + p * naive[j];
        prev[j] = x;
      }
      out[n] = y;
    }
  }
};

// PD, phase distortion (cOscTypePD, 03-oscillators.md): a 32-bit phase bent by the wave's transfer
// function, then read through a cosine. Saw is NS2's (the amount curve with its 0.25 knee
// decompiled); the other shapes follow the Casio CZ with that curve and the documented
// exponents, and the resonant ones window a sine at r = 1 + 16*PD^2 times the pitch [?]. The
// steep segment lasts at least 4 samples instead of NS2's pitch polynomial [?].
struct PhaseDist {
  uint32_t ph = 0;

  void start() { ph = 0; }

  static inline float cosTurn(float t) {  // cos(2*pi*t) from the wave bank's sine
    const int16_t* s = gBank->table[kSine][0];
    float x = (t + 0.25f) * kTableLen;
    x -= floorf(x / kTableLen) * kTableLen;
    const int i = (int)x;
    return (s[i] + (s[i + 1] - s[i]) * (x - (float)i)) * (1.0f / 32767.0f);
  }

  void render(const OscPatch& o, float hz, float amountMod, float* out) {
    const PdWave w = o.pdWave;
    const float pd = fminf(fmaxf(o.pdAmount + amountMod, 0.0f), 1.0f);
    static const float kExp[5] = {0.25f, 0.5f, 0.3f, 0.4f, 0.15f};
    const float a = pd < 0.25f ? 0.08f * pd : 1.3067f * pd - 0.3067f;
    const float dMin = fminf(4.0f * hz / kSampleRate, 0.5f);
    const float d = w < kPdResSaw ? fmaxf(0.5f - 0.5f * powf(fmaxf(a, 0.0f), kExp[w]), dMin) : 0.5f;
    const float rMax = fmaxf(1.0f, 0.45f * kSampleRate / fmaxf(hz, 1.0f));
    const float r = fminf(fminf(1.0f + 16.0f * pd * pd, 64.0f), rMax);
    const uint32_t inc = (uint32_t)(hz * (4294967296.0f / kSampleRate));
    for (int n = 0; n < kBlock; n++) {
      const float phi = (float)ph * (1.0f / 4294967296.0f);
      ph += inc;
      float t;
      switch (w) {
        case kPdSaw: t = phi < d ? 0.5f * phi / d : 0.5f + 0.5f * (phi - d) / (1.0f - d); break;
        case kPdSquare: {
          const float u = phi < 0.5f ? phi : phi - 0.5f, base = phi < 0.5f ? 0.0f : 0.5f;
          t = base + (u < d ? 0.5f * u / d : 0.5f);
          break;
        }
        case kPdPulse: t = phi < 2.0f * d ? phi / (2.0f * d) : 1.0f; break;
        case kPdDoubleSine: {
          const float s = 1.0f - 2.0f * d;  // 0: a sine, 1: two cycles in the first half
          t = (1.0f - s) * phi + s * (phi < 0.5f ? 2.0f * phi : 1.0f);
          break;
        }
        case kPdSawPulse:
          t = phi < 0.5f ? phi : (phi - 0.5f < d ? 0.5f + 0.5f * (phi - 0.5f) / d : 1.0f);
          break;
        default: {  // resonant: a sine at r times the pitch under a per-cycle window
          const float win = w == kPdResSaw ? 1.0f - phi : (w == kPdResFatSaw ? fminf(1.0f, 2.0f * (1.0f - phi))
                                                                       : 1.0f - fabsf(2.0f * phi - 1.0f));
          out[n] = -win * cosTurn(r * phi);
          continue;
        }
      }
      out[n] = cosTurn(t);
    }
  }
};

// ---------------------------------------------------------------------------
// Comb filter (Comb Pos/Neg), as decompiled from NS2 (04-filters.md): the global filter F3, and
// F1/F2 in each voice:
//   tap = delay[sr/Hz]                       (linear interpolation, 2048-sample buffer)
//   h   = hp*h + (tap - tapPrev)             (DC blocker, hp = 1 - 2*pi*5/sr)
//   buf = in + fb*h;   out = dry*in + wet*h
//   fb  = +-q/(q + 0.0618*(1-q)) (max 0.999), dry = 1 - 0.3*CMix, wet = +-dry*CMix
// ---------------------------------------------------------------------------
struct Comb {
  static constexpr int kLen = 2048;
  float* buf = nullptr;
  int idx = 0;
  float delay = -1.0f, tapPrev = 0, h = 0;  // delay < 0: the next block starts at its target

  void init() { buf = (float*)heap_caps_calloc(kLen, sizeof(float), MALLOC_CAP_SPIRAM); }

  void clear() {
    if (buf) memset(buf, 0, sizeof(float) * kLen);
    idx = 0; delay = -1.0f; tapPrev = 0; h = 0;
  }

  // The global filter: its cutoff does not follow the note.
  void process(const CombPatch& p, float* x, float mod, float qMod) {
    run(x, 20.0f * exp2f(9.81378f * (p.cutoff + mod)), Noise::clamp01(p.q + qMod), p.mix, p.negative);
  }

  void run(float* x, float hz, float q, float mix, bool negative) {
    hz = hz < 20.0f ? 20.0f : (hz > 18000.0f ? 18000.0f : hz);
    float target = kSampleRate / hz;
    if (target > kLen - 2) target = kLen - 2;
    if (delay < 0.0f) delay = target;
    const float step = (target - delay) * (1.0f / kBlock);
    const float cmix = Noise::clamp01(mix);
    float fb = q < 1e-6f ? 0.0f : q / (q + 0.0618030f * (1.0f - q));
    if (fb > 0.999f) fb = 0.999f;
    const float dry = 1.0f - 0.3f * cmix;
    float wet = dry * cmix;
    if (negative) { fb = -fb; wet = -wet; }
    const float hp = 1.0f - 6.28318531f * 5.0f / kSampleRate;
    for (int n = 0; n < kBlock; n++) {
      delay += step;
      float pos = (float)idx - delay;
      if (pos < 0.0f) pos += kLen;
      int i0 = (int)pos;
      float frac = pos - (float)i0;
      float tap = buf[i0] + (buf[(i0 + 1) & (kLen - 1)] - buf[i0]) * frac;
      h = hp * h + (tap - tapPrev);
      tapPrev = tap;
      buf[idx] = x[n] + fb * h;
      idx = (idx + 1) & (kLen - 1);
      x[n] = dry * x[n] + wet * h;
    }
  }
};

// The envelopes of a patch by matrix index: Amp, Filter, Pitch, Env 4, Env 5.
static inline const EnvPatch& envPatch(const Patch& p, int i) {
  const EnvPatch* e[kEnvs] = {&p.ampEnv, &p.filtEnv, &p.pitchEnv, &p.env4, &p.env5};
  return *e[i];
}

struct Voice {
  Osc osc[3];
  Noise noise[3];
  SamplePlayer sampler[3];
  FmOsc fm[3];
  NanoSaw nano[3];
  PhaseDist pd[3];
  Filter filter, filter2;
  Comb comb[2];  // F1/F2 as Comb Pos/Neg; Engine::init gives them their buffers
  float combHz[2] = {}, combQ[2] = {};  // their cutoff and Q of this block
  Envelope env[kEnvs];  // Amp, Filter, Pitch, Env 4, Env 5
  EnvScale envSc[kEnvs];  // velocity/key scaling taken at note-on
  Lfo lfo[kLfos];
  Mods mods;  // the matrix of the previous block, for the envelope and LFO parameters
  float src[kSrcCount] = {};  // matrix sources of the last block (the engine reads the newest voice)
  float rand[2] = {}, velocity = 0;
  int note = -1;
  int unisonIdx = 0;
  bool sustained = false;
  bool gate = false;  // key held: a mono note-on glides legato only while the previous key is down
  float pitch = 0, targetPitch = 0, ampStart = 0, detune = 0, slowCut = 0;
  bool fresh = false;  // first block of a note on an idle voice
  bool shed = false;   // dropped for CPU: fades to silence over its next block, then goes idle
  float retrigPrev[kEnvs] = {};  // last block's Retrigger input per envelope
  float gainL = 1, gainR = 1;  // unison pan and level
  float oscGain[3] = {1, 1, 1};  // velocity layer/split gains (velocityGains)
  float posPrev[3] = {-1, -1, -1};  // wavetable position at the end of the previous block
  float levPrev[3] = {-1, -1, -1};  // oscillator level at the end of the previous block
  float phasePrev[3] = {};           // Osc Phase modulation of the previous block

  void shiftPhase(int i, float cycles) {
    const uint32_t d = (uint32_t)(int64_t)(cycles * 4294967296.0);
    osc[i].phase += d;
    for (uint32_t& ph : fm[i].ph) ph += d;
    for (uint32_t& ph : nano[i].ph) ph += d;
    pd[i].ph += d;
  }
  uint32_t age = 0;

  bool active() const { return env[0].stage != Envelope::kIdle; }

  void start(int n, float vel, const Patch& p, const Lfo* shared) {
    const float glide = p.glide;
    for (int e = 0; e < kEnvs; e++) envSc[e] = envScale(envPatch(p, e), n, vel);
    for (float& pp : posPrev) pp = -1.0f;
    for (float& lp : levPrev) lp = -1.0f;
    for (float& pp : phasePrev) pp = 0.0f;
    note = n;
    velocity = vel;
    for (float& r : rand) r = Lfo::uniform(1.0f);
    sustained = false;
    shed = false;
    gate = true;
    targetPitch = (float)n;
    if (glide <= 0.0f || !active()) pitch = targetPitch;
    if (!active()) {
      memset(&mods, 0, sizeof(mods)); slowCut = 0; fresh = true;
      // An idle voice keeps the filter state it had when its envelope ended; with no fade-in that
      // state would start the note with a click.
      filter = Filter(); filter2 = Filter();
      if (Filter::comb(p.filterType)) comb[0].clear();
      if (Filter::comb(p.f2.type)) comb[1].clear();
    }
    // Each note starts at POff: in NS2 two saws a half cycle apart cancel their odd harmonics.
    for (int i = 0; i < 3; i++) osc[i].phase = (uint32_t)(uint64_t)(p.osc[i].phaseOffset * 4294967296.0);  // POff 1 wraps to 0
    velocityGains(p, vel, oscGain);
    for (int i = 0; i < 3; i++) {
      if (p.osc[i].type == kOscSample) sampler[i].start(p.osc[i], n);
      else if (p.osc[i].type == kOscFM) fm[i].start(p.osc[i].fm, n, vel);
      else if (p.osc[i].type == kOscNanoSaw) nano[i].start();
      else if (p.osc[i].type == kOscPD) pd[i].start();
    }
    for (int i = 0; i < kLfos; i++) lfo[i].start(p.lfo[i], shared[i].phase);
    for (int e = 0; e < kEnvs; e++) { env[e].noteOn(envPatch(p, e)); retrigPrev[e] = 0.0f; }
  }

  void release(const Patch& p) {
    gate = false;
    for (int e = 0; e < kEnvs; e++) env[e].noteOff(envPatch(p, e));
    for (int i = 0; i < 3; i++) if (p.osc[i].type == kOscFM) fm[i].release(p.osc[i].fm);
  }

  // `global` holds the engine-wide sources (pitch wheel, knobs, pads) from kSrcPitchWheel on.
  // F1 (k 0) or F2 (k 1) on one bus, with the bus scaled by `gain` first.
  void runFilter(int k, Filter& f, FilterType t, float combMix, float* x, float gain) {
    if (!Filter::comb(t)) { f.run(x, gain); return; }
    for (int n = 0; n < kBlock; n++) x[n] *= gain;
    if (comb[k].buf) comb[k].run(x, combHz[k], combQ[k], combMix, t == kFilterCombNeg);
  }

  void render(const Patch& p, const float* global, float* outL, float* outR) {
    const float dt = (float)kBlock / kSampleRate;
    if (p.glide > 0.0f) {
      float stepSt = 12.0f * dt / p.glide;
      if (pitch < targetPitch) pitch = fminf(pitch + stepSt, targetPitch);
      else pitch = fmaxf(pitch - stepSt, targetPitch);
    } else {
      pitch = targetPitch;
    }

    // Envelopes and LFOs advance with the previous block's matrix (their own parameters as
    // destinations), then the matrix is evaluated with their new outputs. The ones no slot
    // reads are skipped; the amp and filter envelopes always run.
    bool used[kSrcCount] = {};
    used[kSrcEnv1] = used[kSrcEnv1 + 1] = true;
    for (const ModSlot& m : p.mod) used[m.src] = used[m.scale] = true;
    float ampShape = 0, ampLevel = 0;
    for (int e = 0; e < kEnvs; e++) {
      if (!used[kSrcEnv1 + e]) continue;
      const EnvPatch ep = modEnv(envPatch(p, e), mods.env[e]);
      const float shape = env[e].advance(ep, envSc[e].time) * envSc[e].level;
      src[kSrcEnv1 + e] = shape * ep.level;
      if (e == 0) { ampShape = shape; ampLevel = ep.level; }
    }
    for (int i = 0; i < kLfos; i++)
      if (used[kSrcLfo1 + i]) src[kSrcLfo1 + i] = lfo[i].tick(p.lfo[i], mods.lfo[i][0], mods.lfo[i][1]);
    src[kSrcConst] = 1.0f;
    src[kSrcRand1] = rand[0];
    src[kSrcRand2] = rand[1];
    src[kSrcNote] = (float)note * (1.0f / 127.0f);
    src[kSrcVelocity] = velocity;
    src[kSrcGate] = gate ? 1.0f : 0.0f;
    memcpy(src + kSrcPitchWheel, global + kSrcPitchWheel, sizeof(float) * (kSrcCount - kSrcPitchWheel));
    evalMatrix(p, src, mods);
    const float* vm = mods.voice;
    // Retrigger restarts an envelope when its input rises past 0 [I], while the key is held.
    for (int e = 0; e < kEnvs; e++) {
      const float r = mods.env[e][kDstEnvRetrigger - kDstEnvAttack];
      if (gate && r > 0.0f && retrigPrev[e] <= 0.0f) env[e].noteOn(envPatch(p, e));
      retrigPrev[e] = r;
    }

    // Amp Gain adds to the amp envelope's Levl [?]: presets like Arping On set Levl 0 and open
    // the amp from Env 4 -> Amp Gain.
    float ampGain = ampLevel + vm[kDstAmpGain - kDstCutoff];
    ampGain = ampGain < 0.0f ? 0.0f : (ampGain > 2.0f ? 2.0f : ampGain);
    // NS2's amplitude is the square of the envelope times its gain: sustain 0.4 sounds at -16 dB and
    // velocity 40 with VLev 1 at -20 dB, measured against NS2 renders (10-porting-notes.md).
    float aEnd = ampShape * ampGain;
    aEnd *= aEnd;
    if (shed) { aEnd = 0.0f; env[0].stage = Envelope::kIdle; env[0].value = 0; shed = false; }
    // An idle voice starts at its first block's level: NS2 has no fade-in with Attack 0.
    if (fresh) { ampStart = aEnd; fresh = false; }
    float aStep = (aEnd - ampStart) * (1.0f / kBlock);
    float a = ampStart;
    ampStart = aEnd;

    // Cutoff Slow follows its target with a 50 ms one-pole [?]: 1 - exp(-block / 50 ms).
    const float kSlow = 1.0f - 0.97368f;
    slowCut += (vm[kDstCutoffSlow - kDstCutoff] - slowCut) * kSlow;
    float playPitch = pitch + detune;
    float noteHz = 440.0f * exp2f((playPitch - 69.0f) / 12.0f);
    float cutNorm = p.cutoff + p.filterEnvAmt * src[kSrcEnv1 + 1] + vm[0] + slowCut;
    float cutHz = 20.0f * exp2f(9.81378f * cutNorm) * powf(noteHz / 130.8128f, p.keyTrack);
    const float res = Noise::clamp01(p.resonance + vm[kDstQ - kDstCutoff]);
    filter.set(p.filterType, cutHz, res, p.shelfGain, Noise::clamp01(p.drive + vm[kDstDrive - kDstCutoff]), cutNorm, p.vowel);
    combHz[0] = cutHz; combQ[0] = res;
    const bool split = p.route != kRouteStereo, useF2 = split && p.f2.enabled;
    if (useF2) {
      const float cut2 = p.f2.cutoff + p.f2.envAmt * src[kSrcEnv1 + 1] + vm[kDstCutoff2 - kDstCutoff] + slowCut;
      const float hz2 = 20.0f * exp2f(9.81378f * cut2) * powf(noteHz / 130.8128f, p.f2.keyTrack);
      combHz[1] = hz2; combQ[1] = Noise::clamp01(p.f2.resonance + vm[kDstQ2 - kDstCutoff]);
      filter2.set(p.f2.type, hz2, combQ[1], p.f2.shelfGain, Noise::clamp01(p.f2.drive + vm[kDstDrive2 - kDstCutoff]), cut2,
                  p.f2.vowel);
    }

    // Raw oscillator outputs; an oscillator at level 0 still runs when the next one ring-modulates it.
    // Level = clamp(Lev + 2*mod, 0, 2) x velocity gain (03-oscillators.md).
    float raw[3][kBlock], lev[3], ring[3], om[3][kOscDests];
    bool run[3];
    for (int i = 0; i < 3; i++)
      for (int d = 0; d < kOscDests; d++) om[i][d] = mods.osc[i][d] + mods.osc[3][d];
    for (int i = 0; i < 3; i++) {
      const OscPatch& op = p.osc[i];
      const float l = op.level + 2.0f * om[i][kDstOscLevel];
      lev[i] = (l < 0.0f ? 0.0f : (l > 2.0f ? 2.0f : l)) * oscGain[i];
      ring[i] = Noise::clamp01(op.ring + om[i][kDstOscRing]);
    }
    for (int i = 0; i < 3; i++) {
      const OscPatch& op = p.osc[i];
      bool feedsRing = i < 2 && p.osc[i + 1].enabled && ring[i + 1] > 0.0f;
      run[i] = op.enabled && oscGain[i] > 0.0f && (lev[i] != 0.0f || levPrev[i] > 0.0f || feedsRing);
      if (!run[i]) continue;
      // Osc Phase from the matrix moves the phase by the change of its value [I]: Rand -> Phase
      // starts each note's oscillators at their own phase, so stacked ones no longer add up in phase.
      const float po = om[i][kDstOscPhase];
      if (po != phasePrev[i]) { shiftPhase(i, po - phasePrev[i]); phasePrev[i] = po; }
      float oscPitch = 60.0f + (pitch - 60.0f) * op.keyTrack + detune + op.transpose + op.fine +
                       kPitchCoarseRange * om[i][kDstOscPitch] + kPitchFineRange * om[i][kDstOscFine];
      float* r = raw[i];
      if (op.type == kOscSample) {
        sampler[i].render(oscPitch, r);
        continue;
      }
      if (op.type == kOscNanoSaw || op.type == kOscPD) {
        const float hz = 440.0f * exp2f((oscPitch - 69.0f) / 12.0f);
        if (op.type == kOscNanoSaw) nano[i].render(op, hz, om[i][kDstNsDetune], om[i][kDstNsMix], r);
        else pd[i].render(op, hz, om[i][kDstPdDist], r);
        continue;
      }
      if (op.type == kOscFM) {
        fm[i].render(op.fm, 440.0f * exp2f((oscPitch - 69.0f) / 12.0f), &om[i][kDstFmConn], r);
        continue;
      }
      if (op.type == kOscNoise) {
        noise[i].set(op, oscPitch, om[i][kDstNoiseCut], om[i][kDstNoiseQ], om[i][kDstNoiseDeci]);
        for (int n = 0; n < kBlock; n++) r[n] = noise[i].tick();
        continue;
      }
      float hz = 440.0f * exp2f((oscPitch - 69.0f) / 12.0f);
      osc[i].inc = (uint32_t)(hz / kSampleRate * 4294967296.0f);
      int mip = WaveBank::mipFor(hz);
      if (op.type == kOscTable) {
        const Wavetable& t = *op.table;
        const float last = (float)(t.frames - 1);
        float fpEnd = Noise::clamp01(op.position + om[i][kDstWtPos]) * last;
        float fp = posPrev[i] < 0.0f ? fpEnd : posPrev[i];
        const float fpStep = (fpEnd - fp) * (1.0f / kBlock);
        posPrev[i] = fpEnd;
        int f0 = (int)(fp < fpEnd ? fp : fpEnd);
        if (f0 == (int)(fp > fpEnd ? fp : fpEnd) || f0 + 1 >= t.frames) {
          // The whole block stays between the same two frames: only the crossfade moves.
          int f1 = f0 + 1 < t.frames ? f0 + 1 : f0;
          float xf = fp - (float)f0;
          const int16_t *ta = t.data[f0][mip], *tb = t.data[f1][mip];
          for (int n = 0; n < kBlock; n++) { xf += fpStep; r[n] = osc[i].tick2(ta, tb, xf) * t.scale; }
        } else {
          for (int n = 0; n < kBlock; n++) {
            fp += fpStep;
            int g0 = (int)fp;
            int g1 = g0 + 1 < t.frames ? g0 + 1 : g0;
            r[n] = osc[i].tick2(t.data[g0][mip], t.data[g1][mip], fp - (float)g0) * t.scale;
          }
        }
      } else if (op.wave == kPulse) {
        // Pulse: two saw reads pw apart, scaled by the 0.58 found next to it in NS2; it matches NS2's
        // level and spectrum at PWM 0.23 (03-oscillators.md).
        const float pw = fminf(fmaxf(op.pulseWidth + om[i][kDstPulseWidth], 0.01f), 0.99f);
        const float g = gBank->scale[kSaw][mip] * 0.58f;
        const uint32_t width = (uint32_t)(pw * 4294967295.0f);
        for (int n = 0; n < kBlock; n++) r[n] = osc[i].tickPulse(gBank->table[kSaw][mip], width) * g;
      } else if (op.wave == kBiPulse) {
        const float pw = fminf(fmaxf(op.pulseWidth + om[i][kDstPulseWidth], 0.01f), 0.49f);
        // Half the pulse's gain: NS2's level and spectrum at PWM 0.23 [P].
        const float g = gBank->scale[kSaw][mip] * 0.29f;
        const uint32_t width = (uint32_t)(pw * 4294967295.0f);
        for (int n = 0; n < kBlock; n++) r[n] = osc[i].tickBiPulse(gBank->table[kSaw][mip], width) * g;
      } else {
        const int16_t* tab = gBank->table[op.wave][mip];
        const float g = gBank->scale[op.wave][mip];
        for (int n = 0; n < kBlock; n++) r[n] = osc[i].tick(tab) * g;
      }
    }

    // Each oscillator splits between the F1 and F2 buses by FMix (0x1003af0cc); with O1BP, Osc 1
    // skips the filters. In STEREO, F2 runs F1's settings, so both buses go through F1.
    float bus1[kBlock], bus2[kBlock], bypass[kBlock];
    memset(bus1, 0, sizeof(bus1));
    if (split) memset(bus2, 0, sizeof(bus2));
    const bool bypass1 = p.osc1Bypass && run[0];
    if (bypass1) memset(bypass, 0, sizeof(bypass));
    for (int i = 0; i < 3; i++) {
      // The level ramps across the block from where the previous block ended.
      float l = levPrev[i] < 0.0f ? lev[i] : levPrev[i];
      const float lStep = (lev[i] - l) * (1.0f / kBlock);
      levPrev[i] = lev[i];
      if (!run[i] || (lev[i] == 0.0f && l == 0.0f)) continue;
      float* to = i == 0 && bypass1 ? bypass : bus1;
      const float f = split && to == bus1 ? p.osc[i].filterMix : 0.0f;
      const bool ringed = i > 0 && ring[i] > 0.0f && run[i - 1];
      for (int n = 0; n < kBlock; n++, l += lStep) {
        const float x = (ringed ? raw[i][n] * ((1.0f - ring[i]) + ring[i] * raw[i - 1][n]) : raw[i][n]) * l;
        to[n] += x * (1.0f - f);
        if (f != 0.0f) bus2[n] += x * f;
      }
    }

    // Filter outputs: the gains include OGn and the voice convention of half level into the ladder.
    const float g1 = 0.5f * fmaxf(p.filterGain + 2.0f * vm[kDstFilterGain - kDstCutoff], 0.0f);
    runFilter(0, filter, p.filterType, p.combMix, bus1, g1);
    // SERIES adds F1's output to the F2 bus; a disabled F2 passes its bus through.
    const bool series = p.route == kRouteSeries;
    if (useF2) {
      if (series) for (int n = 0; n < kBlock; n++) bus2[n] += 2.0f * bus1[n];
      runFilter(1, filter2, p.f2.type, p.f2.combMix, bus2, 0.5f * fmaxf(p.f2.gain + 2.0f * vm[kDstFilterGain2 - kDstCutoff], 0.0f));
    } else if (split) {
      for (int n = 0; n < kBlock; n++) bus2[n] = 0.5f * bus2[n] + (series ? bus1[n] : 0.0f);
    }
    if (series) memset(bus1, 0, sizeof(bus1));
    const float p1 = p.filterPan, p2 = p.f2.pan;
    // Linear balance law, unity at the centre: In Lazy Curls (F1 and F2 panned hard apart) matches
    // NS2 with it.
    auto panL = [](float x) { return x < 0.5f ? 1.0f : 2.0f * (1.0f - x); };
    auto panR = [](float x) { return x > 0.5f ? 1.0f : 2.0f * x; };
    const float l1 = gainL * panL(p1), r1 = gainR * panR(p1), l2 = gainL * panL(p2), r2 = gainR * panR(p2);
    for (int n = 0; n < kBlock; n++) {
      float yl = bus1[n] * l1, yr = bus1[n] * r1;
      if (split) { yl += bus2[n] * l2; yr += bus2[n] * r2; }
      if (bypass1) { yl += 0.5f * bypass[n] * gainL; yr += 0.5f * bypass[n] * gainR; }
      outL[n] += yl * a;
      outR[n] += yr * a;
      a += aStep;
    }
  }
};

// ---------------------------------------------------------------------------
// Dattorro plate (07-effects.md), buffers in PSRAM.
// ---------------------------------------------------------------------------
struct Line {
  float* buf = nullptr;
  int len = 0, idx = 0;
  void init(float seconds) {
    len = (int)(seconds * kSampleRate) + 2;
    buf = (float*)heap_caps_calloc(len, sizeof(float), MALLOC_CAP_SPIRAM);
  }
  inline float tap(int d) const { int i = idx - d; if (i < 0) i += len; return buf[i]; }
  inline float last() const { return tap(len - 1); }
  inline void push(float x) { buf[idx] = x; if (++idx >= len) idx = 0; }
};

struct AllpassLine {
  Line l;
  float g = 0.5f;
  void init(float seconds, float gain) { l.init(seconds); g = gain; }
  inline float tick(float x) {
    float d = l.last();
    float v = x - g * d;
    l.push(v);
    return d + g * v;
  }
};

// An allpass whose delay swings by +-excursion with a slow sine, read with linear interpolation:
// the modulated tank allpass of Dattorro's plate. The sine is a rotating phasor.
struct ModAllpass {
  Line l;
  float g = 0.7f, base = 0, exc = 0, c = 1, s = 0, kc = 1, ks = 0;
  void init(float seconds, float gain, float excursion, float hz, float phase) {
    l.init(seconds + 2.0f * excursion);
    g = gain;
    base = seconds * kSampleRate;
    exc = excursion * kSampleRate;
    kc = cosf(6.28318531f * hz / kSampleRate);
    ks = sinf(6.28318531f * hz / kSampleRate);
    c = cosf(phase);
    s = sinf(phase);
  }
  inline float tick(float x) {
    const float nc = c * kc - s * ks;
    s = s * kc + c * ks;
    c = nc;
    const float dl = base + exc * s;
    const int i = (int)dl;
    const float a = l.tap(i), d = a + (l.tap(i + 1) - a) * (dl - (float)i);
    const float v = x - g * d;
    l.push(v);
    return d + g * v;
  }
  void renormalize() { const float r = 1.0f / sqrtf(c * c + s * s); c *= r; s *= r; }
};

struct Plate {
  // NS2's tank has 4 modulation LFOs and swings its allpasses by 0.272 ms (07-effects.md) [B];
  // that they drive the 4 tank allpasses, and the ~1 Hz rates (Dattorro's), are [I].
  static constexpr float kExcursion = 0.000272f;
  AllpassLine diff[4];
  ModAllpass apA, apB, ap2A, ap2B;
  Line dA, d2A, dB, d2B;
  float lpA = 0, lpB = 0, bw = 0;
  bool ok = false;

  void init() {
    diff[0].init(0.004762f, 0.75f);
    diff[1].init(0.003583f, 0.75f);
    diff[2].init(0.012721f, 0.625f);
    diff[3].init(0.009297f, 0.625f);
    apA.init(0.022562f, 0.7f, kExcursion, 0.9f, 0.0f); dA.init(0.149433f);
    ap2A.init(0.060408f, 0.5f, kExcursion, 0.7f, 0.8f); d2A.init(0.124830f);
    apB.init(0.030454f, 0.7f, kExcursion, 1.1f, 1.57f); dB.init(0.141519f);
    ap2B.init(0.089138f, 0.5f, kExcursion, 1.3f, 2.4f); d2B.init(0.106145f);
    ok = dA.buf && dB.buf && d2A.buf && d2B.buf && apA.l.buf && apB.l.buf && ap2A.l.buf && ap2B.l.buf;
  }

  void process(const float* in, float* outL, float* outR, float decay, float damp) {
    for (int n = 0; n < kBlock; n++) tick(in[n], decay, damp, outL[n], outR[n]);
    apA.renormalize(); apB.renormalize(); ap2A.renormalize(); ap2B.renormalize();
  }

  inline void tick(float in, float decay, float damp, float& outL, float& outR) {
    bw += 0.7f * (in - bw);
    float x = bw;
    for (auto& d : diff) x = d.tick(x);

    float a = apA.tick(x + d2B.last() * decay);
    dA.push(a);
    lpA += (1.0f - damp) * (dA.last() - lpA);
    float a2 = ap2A.tick(lpA * decay);
    d2A.push(a2);

    float b = apB.tick(x + d2A.last() * decay);
    dB.push(b);
    lpB += (1.0f - damp) * (dB.last() - lpB);
    float b2 = ap2B.tick(lpB * decay);
    d2B.push(b2);

    const float sr = kSampleRate / 29761.0f;
    outL = dB.tap((int)(266 * sr)) + dB.tap((int)(2974 * sr)) - ap2B.l.tap((int)(1913 * sr)) + d2B.tap((int)(1996 * sr)) -
           dA.tap((int)(1990 * sr)) - ap2A.l.tap((int)(187 * sr)) - d2A.tap((int)(1066 * sr));
    outR = dA.tap((int)(353 * sr)) + dA.tap((int)(3627 * sr)) - ap2A.l.tap((int)(1228 * sr)) + d2A.tap((int)(2673 * sr)) -
           dB.tap((int)(2111 * sr)) - ap2B.l.tap((int)(335 * sr)) - d2B.tap((int)(121 * sr));
  }
};

// Freeverb (Jezar), NS2's engine A for Small and Large Room (07-effects.md): 8 low-passed combs and
// 4 allpasses per channel, the right channel's combs 5 % longer or shorter in turn. The input goes
// through the plate's 4 diffusers first: NS2's engine A keeps buffers for them, and without them
// the combs' first echoes come out ~6 dB louder than NS2's rooms [P].
struct Freeverb {
  static constexpr int kCombs = 8;
  static constexpr int kAllpasses = 4;
  Line comb[2][kCombs], ap[2][kAllpasses];
  AllpassLine diff[4];
  float lp[2][kCombs] = {};
  bool ok = false;

  void init() {
    diff[0].init(0.004762f, 0.75f);
    diff[1].init(0.003583f, 0.75f);
    diff[2].init(0.012721f, 0.625f);
    diff[3].init(0.009297f, 0.625f);
    static const int kComb[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int kAp[kAllpasses] = {556, 441, 341, 225};
    ok = true;
    for (int c = 0; c < 2; c++) {
      for (int i = 0; i < kCombs; i++) {
        const float k = c == 0 ? 1.0f : (i & 1 ? 1.05f : 0.95f);
        comb[c][i].init(kComb[i] * k / 44100.0f);
        ok = ok && comb[c][i].buf;
      }
      for (int i = 0; i < kAllpasses; i++) { ap[c][i].init((kAp[i] + 23 * c) / 44100.0f); ok = ok && ap[c][i].buf; }
    }
  }

  inline float channel(int c, float in, float fb, float damp) {
    float out = 0.0f;
    for (int i = 0; i < kCombs; i++) {
      const float y = comb[c][i].last();
      lp[c][i] = y * (1.0f - damp) + lp[c][i] * damp;
      comb[c][i].push(in + lp[c][i] * fb);
      out += y;
    }
    for (int i = 0; i < kAllpasses; i++) {
      const float d = ap[c][i].last();
      ap[c][i].push(out + 0.5f * d);
      out = d - out;
    }
    return out;
  }

  void process(const float* in, float* outL, float* outR, float fb, float damp) {
    for (int n = 0; n < kBlock; n++) {
      float x = in[n] * 0.015f;
      for (auto& d : diff) x = d.tick(x);
      outL[n] = channel(0, x, fb, damp);
      outR[n] = channel(1, x, fb, damp);
    }
  }
};

// ---------------------------------------------------------------------------
// Phaser: chain of first-order allpasses swept by a sine LFO, with feedback; the right channel's
// LFO is offset by Width. Rate/depth ranges are an approximation (07-effects.md marks them [?]).
// ---------------------------------------------------------------------------
struct Delay {
  static constexpr float kMaxSeconds = 6.0f;  // per line, in PSRAM
  Line l, r;
  float lpL = 0, lpR = 0;  // one-pole states of the feedback EQ
  bool ok = false;

  void init() { l.init(kMaxSeconds); r.init(kMaxSeconds); ok = l.buf && r.buf; }

  // In place on the stereo mix, as measured against NS2 renders (07-effects.md) [P]: the dry signal
  // stays at 1 and the echoes come in at 1.1 * Mix^2 (+ the Delay Mix modulation). Stereo keeps
  // each channel on its own line.
  // Cross and Pingpong feed the left line (DTim) only and bounce every echo to the right line
  // (DTim / N), so 1:2 at 250 ms echoes at 250, 375, 625, 750 ms...
  void process(const DelayPatch& p, float mixMod, float* xl, float* xr) {
    const int maxD = l.len - 1;
    const bool cross = p.type != kDelayStereo;
    const float seconds = p.beats > 0.0f ? p.beats * 60.0f / gBpm : p.time;
    int dl = (int)(seconds * kSampleRate), dr = cross ? dl / (p.ratio < 1 ? 1 : p.ratio) : dl;
    dl = dl < 1 ? 1 : (dl > maxD ? maxD : dl);
    dr = dr < 1 ? 1 : (dr > maxD ? maxD : dr);
    // Delay Mix from the matrix adds to the wet level itself, not to Mix: 0 + 0.5 echoes at -6 dB [P].
    const float wet = Noise::clamp01(1.1f * Noise::clamp01(p.mix) * Noise::clamp01(p.mix) + mixMod);
    const float fb = Noise::clamp01(p.feedback);
    // EQCt shelves every pass by up to ~3 dB: below 0.5 the highs from ~400 Hz, above 0.5 the
    // lows under ~700 Hz; 0.5 is flat.
    const float e = Noise::clamp01(p.eq), k = 0.725f * fabsf(e - 0.5f);
    const bool cutHighs = e < 0.5f;
    const float a = cutHighs ? 0.0510f : 0.0876f;  // one-pole at 400 / 700 Hz
    for (int n = 0; n < kBlock; n++) {
      float wl = l.tap(dl), wr = r.tap(dr);
      lpL += a * (wl - lpL);
      lpR += a * (wr - lpR);
      wl -= k * (cutHighs ? wl - lpL : lpL);
      wr -= k * (cutHighs ? wr - lpR : lpR);
      if (cross) {
        l.push(xl[n] + xr[n] + fb * wr);
        r.push(fb * wl);
      } else {
        l.push(xl[n] + fb * wl);
        r.push(xr[n] + fb * wr);
      }
      xl[n] += wl * wet;
      xr[n] += wr * wet;
    }
  }
};

// Chorus, ensemble and flanger: modulated delay lines, one per channel on one LFO. Measured
// against NS2 renders (07-effects.md) [P]: with Chorus 1
// at Dly 0.5 the delay sweeps 6.6..8.6 ms at ~0.3 Hz (Rat 0.25), Mix 1 plays dry 0.66 + wet 0.68 and
// Mix 0.5 dry 0.82 + wet 0.33. Flanger 1 mixes the same way and sits at 2.2 ms with Dly 0.1 and
// 5.2 ms with Dly 0.5; its FB feeds the wet back at full scale. The
// mapping of Dly and Dep elsewhere, and Chorus 2, are [I]. NS2's mono sum fits a single delayed
// copy, so both channels follow the same LFO here and Wid (stereo spread) is left out [I].
struct Chorus {
  Line l, r;
  float phase = 0, phase2 = 0;
  bool ok = false;

  void init() { l.init(0.06f); r.init(0.06f); ok = l.buf && r.buf; }

  static inline float read(const Line& line, float samples) {
    const int i = (int)samples;
    const float f = samples - (float)i;
    return line.tap(i) + (line.tap(i + 1) - line.tap(i)) * f;
  }

  void process(const ChorusPatch& p, float mixMod, float* xl, float* xr) {
    const bool flanger = p.type >= kFlanger1, ensemble = p.type == kEnsemble1 || p.type == kEnsemble2;
    const float spread = p.type == kChorus2 ? 1.5f : 1.0f;
    const float centre = (flanger ? 1.45f + 7.5f * p.delay : 3.0f + 10.0f * p.delay) * 0.001f * kSampleRate;
    const float depth = (flanger ? 3.0f : 3.4f) * p.depth * spread * 0.001f * kSampleRate;
    const float hz = 0.12f * expf(3.36f * p.rate) * (p.type == kChorus2 ? 2.0f : 1.0f);
    const float m = Noise::clamp01(p.mix + mixMod);
    const float fb = Noise::clamp01(p.feedback) * (flanger ? 1.0f : 0.5f);
    const float dry = 1.0f - m / 3.0f, wet = 2.0f * m / 3.0f;
    const float maxD = (float)(l.len - 3);
    // The LFOs move little within a block: the delays are computed at its ends and interpolated.
    auto delayAt = [&](float ph, float c) {
      float d = c + depth * sinf(6.28318531f * ph);
      return d < 1.0f ? 1.0f : (d > maxD ? maxD : d);
    };
    const float step = hz * kBlock / kSampleRate;
    const float d1a = delayAt(phase, centre), d2a = delayAt(phase2, 0.707f * centre);
    phase += step; if (phase >= 1.0f) phase -= 1.0f;
    phase2 += 0.707f * step; if (phase2 >= 1.0f) phase2 -= 1.0f;
    const float d1s = (delayAt(phase, centre) - d1a) / kBlock, d2s = (delayAt(phase2, 0.707f * centre) - d2a) / kBlock;
    for (int n = 0; n < kBlock; n++) {
      const float d1 = d1a + d1s * n, d2 = d2a + d2s * n;
      float wl = read(l, d1), wr = read(r, d1);
      if (ensemble) {
        wl = 0.5f * (wl + read(l, d2));
        wr = 0.5f * (wr + read(r, d2));
      }
      l.push(xl[n] + fb * wl);
      r.push(xr[n] + fb * wr);
      xl[n] = xl[n] * dry + wl * wet;
      xr[n] = xr[n] * dry + wr * wet;
    }
  }
};

struct Phaser {
  static constexpr int kMaxStages = 8;
  float zL[kMaxStages] = {}, zR[kMaxStages] = {};
  float fbL = 0, fbR = 0, lfo = 0;

  static float coef(const PhaserPatch& p, float lfoValue) {
    float hz = 150.0f * exp2f(8.0f * p.depth * (0.5f + 0.5f * lfoValue));
    // Past Nyquist tan() turns negative and the allpass unstable: Dep 1 sweeps up to 38 kHz.
    if (hz > 0.45f * kSampleRate) hz = 0.45f * kSampleRate;
    float t = tanf(3.14159265f * hz / kSampleRate);
    return (t - 1.0f) / (t + 1.0f);
  }

  static inline float chain(float x, float c, float* z, int stages) {
    for (int s = 0; s < stages; s++) {
      float y = c * x + z[s];
      z[s] = x - c * y;
      x = y;
    }
    return x;
  }

  void process(const PhaserPatch& p, const float* inL, const float* inR, float* outL, float* outR) {
    lfo += (0.02f + 4.0f * p.rate * p.rate) * kBlock / kSampleRate;
    if (lfo >= 1.0f) lfo -= 1.0f;
    const float cL = coef(p, sinf(6.28318531f * lfo));
    const float cR = coef(p, sinf(6.28318531f * (lfo + 0.5f * p.width)));
    // Mix 1 plays dry 0.8 + wet 0.8: the level of NS2's Phaser 1/2 renders at FB 0.5 [P].
    const float fb = 0.9f * p.feedback, dryGain = 1.0f - 0.2f * p.mix, wetGain = 0.8f * p.mix;
    const int stages = p.stages < kMaxStages ? p.stages : kMaxStages;
    for (int n = 0; n < kBlock; n++) {
      fbL = chain(inL[n] + fb * fbL, cL, zL, stages);
      fbR = chain(inR[n] + fb * fbR, cR, zR, stages);
      outL[n] = inL[n] * dryGain + fbL * wetGain;
      outR[n] = inR[n] * dryGain + fbR * wetGain;
    }
  }
};

// ---------------------------------------------------------------------------
// Engine: voice allocation, block rendering, multi-FX, reverb.
// ---------------------------------------------------------------------------
// One hit of a Slate pad: its samples summed, filtered, shaped by the envelope and panned.
struct DrumVoice {
  static constexpr int kChokeBlocks = 3;  // a choked hit fades out over ~4 ms
  const KitPad* pad = nullptr;
  const Sample* smp[kPadSamples] = {};
  int64_t pos[kPadSamples] = {};  // 32.32 positions; reverse samples step backwards
  int64_t step[kPadSamples] = {};
  float gain[kPadSamples] = {};
  float level = 0, ampStart = 0, t = 0, gainL = 1, gainR = 1;
  // A pad or bus filter: a one-pole for the 6 dB types, a biquad for the others.
  struct PadFilter {
    uint8_t type = 0;
    bool on = false;
    float a = 0, z = 0;
    Biquad bq;
    void set(bool enabled, uint8_t t, float cutoff, float q) {
      on = enabled;
      type = t;
      z = 0;
      if (!on) return;
      const float hz = 20.0f * exp2f(9.81378f * Noise::clamp01(cutoff));
      a = 1.0f - expf(-6.28318531f * fminf(hz, 0.45f * kSampleRate) / kSampleRate);
      bq = Biquad();
      const float bq_q = 0.5f + 5.5f * q * q;
      if (t == 1) bq.set(kNoiseLowPass, hz, bq_q);
      else if (t == 3) bq.set(kNoiseHighPass, hz, bq_q);
      else if (t == 4) bq.set(kNoiseBandPass, hz, bq_q);
    }
    void run(float* x) {
      if (!on) return;
      if (type == 0 || type == 2) {
        for (int n = 0; n < kBlock; n++) {
          z += a * (x[n] - z);
          x[n] = type == 2 ? x[n] - z : z;
        }
      } else {
        bq.run(x);
      }
    }
  };
  PadFilter filter, bus;
  uint32_t age = 0;
  int choke = 0;  // blocks left of a choke fade, 0 = none
  bool active = false;

  void start(const KitPad& p, const Sample* samples, float vel) {
    pad = &p;
    for (int k = 0; k < kPadSamples; k++) {
      const KitSample& ks = p.s[k];
      smp[k] = ks.sample >= 0 ? &samples[ks.sample] : nullptr;
      if (!smp[k] || smp[k]->length < 2) { smp[k] = nullptr; continue; }
      const double rate = smp[k]->rate * exp2((p.pitch + ks.pitch) / 12.0) * 4294967296.0;
      const int64_t start = (int64_t)(Noise::clamp01(ks.offset) * (smp[k]->length - 2)) << 32;
      step[k] = ks.reverse ? -(int64_t)rate : (int64_t)rate;
      pos[k] = ks.reverse ? ((int64_t)(smp[k]->length - 2) << 32) - start : start;
      // x0.9: the factory samples peak near full scale; this keeps the 808 kick at velocity 100
      // where the board's output has headroom.
      gain[k] = ks.level * (0.9f / 32768.0f);
    }
    // Level and velocity count squared, the velocity curved: fitted to NS2's 808 kit (pads at Lev
    // 0.45-1.15, kick at velocity 40/90/127 within 1 dB) [P].
    const float velScale = 1.0f - p.velLevel + p.velLevel * vel * vel;
    level = p.level * p.level * velScale * velScale;
    const float pan = Noise::clamp01(p.pan);
    gainL = pan > 0.5f ? 2.0f * (1.0f - pan) : 1.0f;  // linear balance, unity at the centre
    gainR = pan < 0.5f ? 2.0f * pan : 1.0f;
    filter.set(p.filterOn, p.filterType, p.cutoff + p.cutoffVel * vel, p.q);
    bus.set(p.busFilterOn, p.busFilterType, p.busCutoff, p.busQ);
    t = 0;
    ampStart = envelope(0.0f);
    choke = 0;
    active = true;
  }

  // The envelope's level at time t: attack, hold, decay to the break level, decay 2 to silence. The
  // decays fall linearly in dB, 42 dB over their time, then cut: NS2's 808 kit pads with decay 2
  // of 1 s and 0.24 s end within 10 ms of it [P]; the first decay's shape is [I].
  float envelope(float at) const {
    const KitPad& p = *pad;
    if (at < p.attack) return at / p.attack;
    at -= p.attack;
    if (at < p.hold || p.decay < 0.0f) return 1.0f;
    at -= p.hold;
    if (at < p.decay) return p.breakLevel + (1.0f - p.breakLevel) * fall(at / p.decay);
    at -= p.decay > 0.0f ? p.decay : 0.0f;
    if (p.decay2 < 0.0f) return p.breakLevel;
    return at < p.decay2 ? p.breakLevel * fall(at / p.decay2) : 0.0f;
  }

  // 1 down to -42 dB over x = 0..1.
  static float fall(float x) { return exp2f(-6.9762f * x); }

  // Adds the hit to the dry mix and, by the pad's bus send, to the reverb input.
  void render(float* outL, float* outR, float* send) {
    float x[kBlock] = {};
    bool playing = false;
    for (int k = 0; k < kPadSamples; k++) {
      if (!smp[k]) continue;
      const Sample& sm = *smp[k];
      const int64_t last = (int64_t)(sm.length - 1) << 32;
      int64_t ps = pos[k];
      const int64_t st = step[k];
      const float g = gain[k];
      int n = 0;
      for (; n < kBlock && ps >= 0 && ps < last; n++, ps += st) {
        const uint32_t i = (uint32_t)(ps >> 32);
        const float f = (float)(uint32_t)ps * (1.0f / 4294967296.0f);
        x[n] += g * (sm.data[i] + (sm.data[i + 1] - sm.data[i]) * f);
      }
      pos[k] = ps;
      if (n < kBlock) smp[k] = nullptr;
      else playing = true;
    }
    filter.run(x);
    bus.run(x);
    t += (float)kBlock / kSampleRate;
    float aEnd = envelope(t);
    if (choke) aEnd *= (float)(--choke) / kChokeBlocks;
    float a = ampStart * level;
    const float aStep = (aEnd - ampStart) * level * (1.0f / kBlock);
    // The send takes the mono hit as the voices' reverb input takes the mix ((L + R) / 4).
    const float sendGain = 0.5f * pad->send;
    for (int n = 0; n < kBlock; n++, a += aStep) {
      outL[n] += x[n] * a * gainL;
      outR[n] += x[n] * a * gainR;
      send[n] += x[n] * a * sendGain;
    }
    ampStart = aEnd;
    if (!playing || aEnd <= 0.0f) active = false;
  }
};

struct Engine {
  static constexpr int kDrumVoices = 16;
  Patch patch;
  Voice voices[kVoices];
  DrumVoice drums[kDrumVoices];
  float kitSend[kBlock] = {};  // the kit's bus sends of this block, the reverb input in a kit
  Plate plate;
  Freeverb room;
  Line reverbPre;               // pre-delay, up to the Hall's 350 ms
  float revHp[2] = {}, revLp[2] = {};  // one-pole HCut/LCut states of the wet channels
  Phaser phaser;
  Chorus chorus;
  Delay delay;
  Comb combL, combR;  // the global filter is stereo
  Filter f3L, f3R;
  float dcIn[2] = {}, dcOut[2] = {};  // DC blocker states of the voice mix
  Lfo lfo[kLfos];  // free-running LFOs for the engine-wide destinations
  uint32_t clock = 0;
  float wheel = 0;            // pitch wheel, -1..1
  float macro[kMacros] = {};  // knob and pad positions (matrix sources Knob 1..8, Pad X/Y)
  float global[5] = {};       // engine-wide matrix sums of the last block (Mods::global)
  bool sustain = false;       // CC64

  void init() {
    plate.init(); room.init(); reverbPre.init(0.35f); combL.init(); combR.init(); delay.init(); chorus.init();
    for (auto& v : voices) { v.comb[0].init(); v.comb[1].init(); }
    for (int i = 0; i < kLfos; i++) lfo[i].start(patch.lfo[i], 0.0f);
  }

  // Loads a patch; the knobs and pads jump to the positions the preset was saved with.
  void setPatch(const Patch& p) {
    if (p.kit != patch.kit) chokeDrums(0);
    patch = p;
    memcpy(macro, p.macro, sizeof(macro));
  }

  void noteOn(int note, int vel) {
    if (vel == 0) { noteOff(note); return; }
    if (patch.kit) { drumOn(note, vel); return; }
    const int uni = patch.unison < 1 ? 1 : (patch.unison > 4 ? 4 : patch.unison);
    if (patch.mono) {  // voices 0..uni-1 play the one note, stacked in unison
      const bool legato = voices[0].active() && voices[0].gate;
      for (int u = 0; u < uni; u++) {
        Voice& v = voices[u];
        if (legato) {
          v.note = note;
          v.targetPitch = (float)note;
          v.sustained = false;
          velocityGains(patch, (float)vel / 127.0f, v.oscGain);  // legato keeps the playing zones
        } else {
          v.unisonIdx = u;
          setUnison(v, uni, u);
          v.start(note, (float)vel / 127.0f, patch, lfo);
        }
      }
      return;
    }
    for (int u = 0; u < uni; u++) {
      Voice* pick = nullptr;
      for (auto& v : voices) if (v.note == note && v.active() && v.unisonIdx == u) pick = &v;
      if (!pick) for (auto& v : voices) if (!v.active()) { pick = &v; break; }
      if (!pick) {
        pick = &voices[0];
        for (auto& v : voices) if (v.age < pick->age) pick = &v;
      }
      pick->age = ++clock;
      pick->unisonIdx = u;
      setUnison(*pick, uni, u);
      pick->start(note, (float)vel / 127.0f, patch, lfo);
    }
  }

  // Unison voice u of n (decompiled tables, cUnisonVoice): detune in semitones
  // det*UDtn^2*(1 + 0.2n), pan 0.5 + pan*USte, level scale per voice count.
  void setUnison(Voice& v, int n, int u) const {
    static const float kScale[5] = {1.0f, 1.0f, 0.85f, 0.75f, 0.7f};
    static const float kDet[5][4] = {{0}, {0}, {-1, 1}, {-1, 0, 1}, {-1, -0.5f, 0.5f, 1}};
    static const float kPan[5][4] = {{0}, {0}, {-0.5f, 0.5f}, {-0.5f, 0, 0.5f}, {-0.5f, 0.25f, -0.25f, 0.5f}};
    float d = Noise::clamp01(patch.unisonDetune);
    v.detune = kDet[n][u] * d * d * (1.0f + 0.2f * n);
    float pan = Noise::clamp01(0.5f + kPan[n][u] * Noise::clamp01(patch.unisonStereo));
    // The level scale counts squared, like the amplitude it scales: 2 detuned voices sum to the
    // level of one in NS2 renders (0.85^2 * sqrt 2 = 1.02). Constant-sum pan law: L + R stays 2,
    // as in NS2 renders, where an oscillator panned anywhere keeps the mono sum's level [P].
    const float g = kScale[n] * kScale[n];
    v.gainL = g * 2.0f * (1.0f - pan);
    v.gainR = g * 2.0f * pan;
  }

  // Every pad of the note starts a hit; a pad's group keeps at most its polyphony of hits, choking
  // the oldest (open and closed hi-hats share one).
  void drumOn(int note, int vel) {
    const Kit& kit = *patch.kit;
    for (int i = 0; i < kKitPads; i++) {
      const KitPad& p = kit.pad[i];
      if (p.note != note) continue;
      const int poly = p.group ? kit.groupPoly[p.group] : 0;
      if (poly > 0) {
        int playing = 0;
        for (auto& d : drums) playing += d.active && !d.choke && d.pad->group == p.group;
        for (; playing >= poly; playing--) {
          DrumVoice* oldest = nullptr;
          for (auto& d : drums)
            if (d.active && !d.choke && d.pad->group == p.group && (!oldest || d.age < oldest->age)) oldest = &d;
          oldest->choke = DrumVoice::kChokeBlocks;
        }
      }
      DrumVoice* pick = nullptr;
      for (auto& d : drums) if (!d.active) { pick = &d; break; }
      if (!pick) {
        pick = &drums[0];
        for (auto& d : drums) if (d.age < pick->age) pick = &d;
      }
      pick->age = ++clock;
      pick->start(p, patch.kitSamples, (float)vel / 127.0f);
    }
  }

  // Fades out the hits of a group (0: all of them).
  void chokeDrums(int group) {
    for (auto& d : drums) if (d.active && (!group || d.pad->group == group)) d.choke = DrumVoice::kChokeBlocks;
  }

  void noteOff(int note) {
    if (patch.kit) return;  // one-shot pads play out
    for (auto& v : voices) {
      if (v.note != note || !v.active() || v.env[0].stage == Envelope::kRelease) continue;
      if (sustain) v.sustained = true;
      else v.release(patch);
    }
  }

  void setSustain(bool on) {
    sustain = on;
    if (on) return;
    for (auto& v : voices) if (v.sustained) { v.sustained = false; v.release(patch); }
  }

  void allOff() {
    for (auto& v : voices) { v.sustained = false; v.release(patch); }
    chokeDrums(0);
  }

  int activeVoices() const {
    int n = 0;
    for (auto& v : voices) n += v.active();
    for (auto& d : drums) n += d.active;
    return n;
  }

  // Overload protection: fades the oldest sounding voice out over one block (a cut to zero
  // clicks). Called when a block took more than the audio budget, so a heavy preset drops notes
  // instead of starving the MIDI loop.
  void shedOldest() {
    Voice* oldest = nullptr;
    for (auto& v : voices) if (v.active() && !v.shed && (!oldest || v.age < oldest->age)) oldest = &v;
    if (oldest) oldest->shed = true;
  }

  // Dry mono mix of all voices for one block.
  void renderVoices(float* dryL, float* dryR) {
    memset(dryL, 0, sizeof(float) * kBlock);
    memset(dryR, 0, sizeof(float) * kBlock);
    float src[kSrcCount] = {};
    src[kSrcPitchWheel] = wheel;
    src[kSrcPitchWheelUp] = wheel > 0.0f ? wheel : 0.0f;
    src[kSrcPitchWheelDown] = wheel < 0.0f ? -wheel : 0.0f;
    memcpy(src + kSrcMacro1, macro, sizeof(macro));
    memset(kitSend, 0, sizeof(kitSend));
    for (auto& d : drums) if (d.active) d.render(dryL, dryR, kitSend);
    if (patch.kit) {
      blockDc(dryL, 0);
      blockDc(dryR, 1);
      return;
    }
    const Voice* newest = nullptr;
    for (auto& v : voices) {
      if (!v.active()) continue;
      v.render(patch, src, dryL, dryR);
      if (!newest || v.age > newest->age) newest = &v;
    }
    updateGlobal(newest, src);
    if (patch.comb.enabled && combL.buf && combR.buf) {
      combL.process(patch.comb, dryL, global[0], global[1]);
      combR.process(patch.comb, dryR, global[0], global[1]);
    } else if (patch.f3.enabled) {
      // The global filters do not follow the note: their KeyT is 1 in every preset, and NS2 renders of
      // Rave Lead (HP 24-A) and Bold as Brass (LP 12-A) only match without keytracking.
      const GlobalFilterPatch& f = patch.f3;
      float hz = 20.0f * exp2f(9.81378f * (f.cutoff + global[0]));
      hz = hz < 20.0f ? 20.0f : hz;
      const float q = Noise::clamp01(f.q + global[1]);
      f3L.set(f.type, hz, q, f.shelfGain, f.drive, f.cutoff + global[0], f.vowel);
      f3R.set(f.type, hz, q, f.shelfGain, f.drive, f.cutoff + global[0], f.vowel);
      // The ladder sees half the mix, like F1, and the output is brought back to unity. A waveshaper
      // here drives 5x less and comes out 5x louder than in a voice (fitted, 04-filters.md) [P]; its
      // dry share stays at unity (0.4 * 0.5 * 5).
      if (Filter::shaper(f.type)) {
        for (int n = 0; n < kBlock; n++) { dryL[n] *= kGlobalWsIn; dryR[n] *= kGlobalWsIn; }
        f3L.run(dryL, 0.5f * f.gain);
        f3R.run(dryR, 0.5f * f.gain);
        for (int n = 0; n < kBlock; n++) { dryL[n] *= kGlobalWsOut; dryR[n] *= kGlobalWsOut; }
      } else {
        f3L.run(dryL, 0.5f * f.gain);
        f3R.run(dryR, 0.5f * f.gain);
        for (int n = 0; n < kBlock; n++) { dryL[n] *= 2.0f; dryR[n] *= 2.0f; }
      }
    }
    blockDc(dryL, 0);
    blockDc(dryR, 1);
  }

  // NS2's output carries no DC: an FM Saw Down with feedback, whose mean is far from zero, comes out
  // centred with the offset decaying at tau = 32 ms, a one-pole high-pass at ~5 Hz (04-filters.md) [P].
  void blockDc(float* x, int ch) {
    constexpr float kPole = 1.0f - 2.0f * (float)M_PI * 5.0f / kSampleRate;
    float in = dcIn[ch], out = dcOut[ch];
    for (int n = 0; n < kBlock; n++) {
      out = kPole * out + x[n] - in;
      in = x[n];
      x[n] = out;
    }
    dcIn[ch] = in;
    dcOut[ch] = out;
  }

  static constexpr float kGlobalWsIn = 0.4f, kGlobalWsOut = 5.0f;

  // Engine-wide destinations have no owning voice: their per-voice sources come from the newest
  // voice and their LFOs from the engine's own free-running ones [?].
  void updateGlobal(const Voice* newest, const float* globalSrc) {
    float src[kSrcCount];
    memcpy(src, newest ? newest->src : globalSrc, sizeof(src));
    for (int i = 0; i < kLfos; i++) src[kSrcLfo1 + i] = lfo[i].tick(patch.lfo[i], 0.0f, 0.0f);
    Mods m;
    evalMatrix(patch, src, m);
    memcpy(global, m.global, sizeof(global));
  }

  float reverbMix() const { return Noise::clamp01(patch.reverbMix + global[2]); }

  // The reverb's mono input for a block of the voice mix: the mix itself, or a kit's bus sends.
  void reverbInput(const float* dryL, const float* dryR, float* in) const {
    if (patch.kit) memcpy(in, kitSend, sizeof(kitSend));
    else for (int n = 0; n < kBlock; n++) in[n] = (dryL[n] + dryR[n]) * 0.25f;
  }
  bool reverbReady() const { return (patch.reverbRoom ? room.ok : plate.ok) && reverbPre.buf; }

  // The wet block for a mono input block (core 0): pre-delay crossfade, plate or room, then HCut as
  // a one-pole high-pass and LCut as a one-pole low-pass, at the wet level of Mix 1.
  void reverb(const float* in, float* wl, float* wr) {
    const Patch& p = patch;
    float x[kBlock];
    const int pre = (int)(p.reverbPre * kSampleRate);
    for (int n = 0; n < kBlock; n++) {
      reverbPre.push(in[n]);
      x[n] = pre > 0 ? in[n] + p.reverbPreMix * (reverbPre.tap(pre < reverbPre.len - 1 ? pre : reverbPre.len - 1) - in[n]) : in[n];
    }
    if (p.reverbRoom) room.process(x, wl, wr, p.reverbDecay, p.reverbDamp);
    else plate.process(x, wl, wr, p.reverbDecay, p.reverbDamp);
    const float hp = 1.0f - expf(-6.28318531f * p.reverbHpHz / kSampleRate);
    const float lp = 1.0f - expf(-6.28318531f * p.reverbLpHz / kSampleRate);
    float* ch[2] = {wl, wr};
    for (int c = 0; c < 2; c++)
      for (int n = 0; n < kBlock; n++) {
        revHp[c] += hp * (ch[c][n] - revHp[c]);
        revLp[c] += lp * ((ch[c][n] - revHp[c]) - revLp[c]);
        ch[c][n] = revLp[c] * p.reverbGain;
      }
    for (int n = 0; n < kBlock; n++) {
      const float m = 0.5f * (wl[n] + wr[n]), side = 0.5f * p.reverbWidth * (wl[n] - wr[n]);
      wl[n] = m + side;
      wr[n] = m - side;
    }
  }

  // Multi-FX (phaser or chorus) then Delay, in place on the stereo voice mix (the reverb follows on core 0).
  void multiFx(float* l, float* r) {
    if (patch.phaser.enabled) {
      PhaserPatch ph = patch.phaser;
      ph.mix = Noise::clamp01(ph.mix + global[3]);
      phaser.process(ph, l, r, l, r);
    }
    if (patch.chorus.enabled && chorus.ok) chorus.process(patch.chorus, global[3], l, r);
    if (patch.delay.enabled && delay.ok) delay.process(patch.delay, global[4], l, r);
  }

  // Stereo-linked peak limiter with one block of lookahead: the gain is down to what the loudest
  // sample needs before that sample plays, and recovers over ~80 ms, so loud chords are turned down
  // instead of clipped. The output is one block (1.3 ms) late.
  struct Limiter {
    static constexpr float kCeiling = 0.89f;                      // -1 dBFS
    static constexpr float kRelease = 1.0f - 0.98348f;            // 1 - exp(-block / 80 ms)
    float l[kBlock] = {}, r[kBlock] = {};
    float need = 1.0f, g = 1.0f;  // gain the delayed block needs; gain at the end of the last block

    void process(float* L, float* R) {
      float next = 1.0f;
      for (int n = 0; n < kBlock; n++) {
        const float peak = fmaxf(fabsf(L[n]), fabsf(R[n]));
        if (peak * next > kCeiling) next = kCeiling / peak;
      }
      const float target = fminf(need, next);
      const float end = target < g ? target : g + (target - g) * kRelease;
      const float step = (end - g) * (1.0f / kBlock);
      float a = g;
      for (int n = 0; n < kBlock; n++, a += step) {
        const float dl = l[n], dr = r[n];
        l[n] = L[n];
        r[n] = R[n];
        L[n] = dl * a;
        R[n] = dr * a;
      }
      g = end;
      need = next;
    }
  };
  Limiter limiter;

  // Linear up to kClipKnee, then bends smoothly (slope 1 at the knee) towards full scale.
  static float softClip(float x) {
    const float a = fabsf(x);
    if (a <= kClipKnee) return x;
    const float u = (a - kClipKnee) / (1.0f - kClipKnee);
    return copysignf(kClipKnee + (1.0f - kClipKnee) * u / (1.0f + u), x);
  }

  // Mixes dry + (already computed) wet, applies volume, the limiter and the soft clip, writes 16-bit stereo.
  void mixOut(const float* dryL, const float* dryR, const float* wetL, const float* wetR, int16_t* out) {
    // NS2's mix keeps the dry at full level up to Mix 0.5 and fades it out above (07-effects.md).
    const float wet = reverbReady() ? reverbMix() : 0.0f, dryGain = fminf(1.0f, 2.0f * (1.0f - wet));
    float l[kBlock], r[kBlock];
    for (int n = 0; n < kBlock; n++) {
      l[n] = (dryL[n] * dryGain + wetL[n] * wet) * patch.volume * kOutputTrim;
      r[n] = (dryR[n] * dryGain + wetR[n] * wet) * patch.volume * kOutputTrim;
    }
    limiter.process(l, r);
    for (int n = 0; n < kBlock; n++) {
      out[2 * n] = (int16_t)(softClip(l[n]) * 32767.0f);
      out[2 * n + 1] = (int16_t)(softClip(r[n]) * 32767.0f);
    }
  }
};

}  // namespace syn

#pragma GCC pop_options
