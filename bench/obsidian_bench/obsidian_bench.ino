// Cycle-count benchmark of the Obsidian DSP building blocks described in docs/obsidian.
// Prints cycles per sample for each block and an estimate of voices per core at 48 kHz.

#include <Arduino.h>
#include <math.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <functional>

static constexpr float kSampleRate = 48000.0f;
static constexpr int kBlock = 64;
static constexpr int kBlocks = 2000;
static constexpr int kTableLen = 1024;

static int16_t gTables[2][kTableLen + 1];
static float gSine512[513];
static float gIn[kBlock], gOut[kBlock], gOutR[kBlock];

// Wavetable oscillator: 32-bit phase, linear interpolation, crossfade between two frames.
struct WtOsc {
  uint32_t phase = 0, inc = 0;
  float morph = 0.3f;
  void setFreq(float hz) { inc = (uint32_t)(hz / kSampleRate * 4294967296.0f); }
  inline float tick() {
    uint32_t idx = phase >> 22;
    float frac = (float)((phase >> 6) & 0xffff) * (1.0f / 65536.0f);
    float a0 = gTables[0][idx], a1 = gTables[0][idx + 1];
    float b0 = gTables[1][idx], b1 = gTables[1][idx + 1];
    float a = a0 + (a1 - a0) * frac, b = b0 + (b1 - b0) * frac;
    phase += inc;
    return (a + (b - a) * morph) * (1.0f / 32768.0f);
  }
};

// DPW sawtooth (NanoSaw building block).
struct DpwSaw {
  float phase = 0, inc = 0, prev = 0, scale = 0;
  void setFreq(float hz) { inc = hz / kSampleRate; scale = 1.0f / (4.0f * inc * (1.0f - inc)); }
  inline float tick() {
    phase += inc;
    if (phase >= 1.0f) phase -= 1.0f;
    float p = 2.0f * phase - 1.0f, par = p * p;
    float out = (par - prev) * scale;
    prev = par;
    return out;
  }
};

struct NanoSaw {
  DpwSaw saws[7];
  float gains[7] = {0.5f, 0.3f, 0.3f, 0.25f, 0.25f, 0.2f, 0.2f};
  void setFreq(float hz, float d) {
    const float off[7] = {0, -0.11f * d, 0.11f * d, -0.4f * d, 0.4f * d, -d, d};
    for (int i = 0; i < 7; i++) saws[i].setFreq(hz * (1.0f + off[i] * 0.05f));
  }
  inline float tick() {
    float s = 0;
    for (int i = 0; i < 7; i++) s += saws[i].tick() * gains[i];
    return s;
  }
};

// 4-pole ladder: global feedback k*y4, clip-with-leak saturation, 4 one-pole stages, LP24 tap.
struct Ladder {
  float y1 = 0, y2 = 0, y3 = 0, y4 = 0, g = 0.1f, k = 2.5f, lo = -1.2f, hi = 1.2f, leak = 0.1f;
  void setCutoff(float hz) { float w = tanf(3.14159265f * hz / kSampleRate); g = w / (1.0f + w); }
  inline float tick(float x) {
    float u = x - k * y4;
    float s = fminf(fmaxf(u, lo), hi) + leak * u;
    y1 += g * (s - y1);
    y2 += g * (y1 - y2);
    y3 += g * (y2 - y3);
    y4 += g * (y3 - y4);
    return 0.75f * y4;
  }
};

struct Biquad {
  float b0 = 0.2f, b1 = 0.4f, b2 = 0.2f, a1 = -0.6f, a2 = 0.2f, z1 = 0, z2 = 0;
  inline float tick(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// Envelope segment with the rational curve y0 + d*t/(t + (k-1)(1-t)).
struct Env {
  float t = 0, rate = 1.0f / (0.5f * kSampleRate), y0 = 0, y1 = 1, km1 = 0.22051f;
  inline float tick() {
    t += rate;
    if (t >= 1.0f) { t = 0.0f; float tmp = y0; y0 = y1; y1 = tmp; }
    return y0 + (y1 - y0) * t / (t + km1 * (1.0f - t));
  }
};

struct Lfo {
  float phase = 0, inc = 2.0f / kSampleRate;
  inline float tick() {
    phase += inc;
    if (phase >= 1.0f) phase -= 1.0f;
    float pos = phase * 512.0f;
    int i = (int)pos;
    float f = pos - (float)i;
    return gSine512[i] + (gSine512[i + 1] - gSine512[i]) * f;
  }
};

// One Obsidian-like voice: 3 wavetable oscillators, 2 ladders, amp/filter envelopes, 1 LFO.
// Cutoff (exp2f) is recomputed once per block, as NS2 does at control rate.
struct Voice {
  WtOsc osc[3];
  Ladder f1, f2;
  Env ampEnv, filtEnv;
  Lfo lfo;
  void init(float hz) { for (int i = 0; i < 3; i++) osc[i].setFreq(hz * (1.0f + 0.003f * i)); }
  void block(float* out) {
    float cut = 20.0f * exp2f(9.8138f * (0.4f + 0.3f * filtEnv.tick() + 0.05f * lfo.tick()));
    f1.setCutoff(cut);
    f2.setCutoff(cut * 1.5f);
    for (int n = 0; n < kBlock; n++) {
      float s = osc[0].tick() + osc[1].tick() + osc[2].tick();
      float y = f2.tick(f1.tick(s));
      out[n] += y * ampEnv.tick();
    }
  }
};

// Same voice with the two hot spots reworked for Xtensa: envelopes evaluated once per
// block and interpolated, and the ladder clip done with compares instead of fminf/fmaxf.
struct FastLadder {
  float y1 = 0, y2 = 0, y3 = 0, y4 = 0, g = 0.1f, k = 2.5f, lo = -1.2f, hi = 1.2f, leak = 0.1f;
  void setCutoff(float hz) { float w = tanf(3.14159265f * hz / kSampleRate); g = w / (1.0f + w); }
  inline float tick(float x) {
    float u = x - k * y4;
    float c = u < lo ? lo : (u > hi ? hi : u);
    float s = c + leak * u;
    y1 += g * (s - y1);
    y2 += g * (y1 - y2);
    y3 += g * (y2 - y3);
    y4 += g * (y3 - y4);
    return 0.75f * y4;
  }
};

struct BlockEnv {
  Env env;
  float last = 0;
  inline void next(float& start, float& step) {
    env.rate = 1.0f / (0.5f * kSampleRate) * kBlock;
    float v = env.tick();
    start = last;
    step = (v - last) * (1.0f / kBlock);
    last = v;
  }
};

struct FastVoice {
  WtOsc osc[3];
  FastLadder f1, f2;
  BlockEnv ampEnv, filtEnv;
  Lfo lfo;
  void init(float hz) { for (int i = 0; i < 3; i++) osc[i].setFreq(hz * (1.0f + 0.003f * i)); }
  void block(float* out) {
    float fs, fstep, a, astep;
    filtEnv.next(fs, fstep);
    ampEnv.next(a, astep);
    lfo.inc = 2.0f / kSampleRate * kBlock;
    float cut = 20.0f * exp2f(9.8138f * (0.4f + 0.3f * fs + 0.05f * lfo.tick()));
    f1.setCutoff(cut);
    f2.setCutoff(cut * 1.5f);
    for (int n = 0; n < kBlock; n++) {
      float s = osc[0].tick() + osc[1].tick() + osc[2].tick();
      float y = f2.tick(f1.tick(s));
      out[n] += y * a;
      a += astep;
    }
  }
};

// Freeverb: 8 LBCF combs + 4 allpasses per channel.
struct Comb {
  float* buf; int len, idx = 0; float store = 0, fb = 0.84f, damp = 0.2f;
  inline float tick(float x) {
    float y = buf[idx];
    store = y * (1.0f - damp) + store * damp;
    buf[idx] = x + store * fb;
    if (++idx >= len) idx = 0;
    return y;
  }
};
struct Allpass {
  float* buf; int len, idx = 0; float g = 0.5f;
  inline float tick(float x) {
    float b = buf[idx], y = -x + b;
    buf[idx] = x + b * g;
    if (++idx >= len) idx = 0;
    return y;
  }
};

static float* allocBuf(int n, bool psram) {
  void* p = psram ? heap_caps_calloc(n, sizeof(float), MALLOC_CAP_SPIRAM)
                  : heap_caps_calloc(n, sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return (float*)p;
}

struct Freeverb {
  Comb c[16];
  Allpass a[8];
  bool ok = true;
  void init(bool psram) {
    const int cl[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    const int al[4] = {556, 441, 341, 225};
    for (int i = 0; i < 16; i++) {
      c[i].len = (int)(cl[i & 7] * (kSampleRate / 44100.0f) * (i < 8 ? 1.0f : 1.05f));
      c[i].buf = allocBuf(c[i].len, psram);
      ok &= c[i].buf != nullptr;
    }
    for (int i = 0; i < 8; i++) {
      a[i].len = (int)(al[i & 3] * (kSampleRate / 44100.0f) + (i < 4 ? 0 : 23));
      a[i].buf = allocBuf(a[i].len, psram);
      ok &= a[i].buf != nullptr;
    }
  }
  void block(const float* in, float* l, float* r) {
    for (int n = 0; n < kBlock; n++) {
      float x = in[n] * 0.015f, sl = 0, sr = 0;
      for (int i = 0; i < 8; i++) sl += c[i].tick(x);
      for (int i = 8; i < 16; i++) sr += c[i].tick(x);
      for (int i = 0; i < 4; i++) sl = a[i].tick(sl);
      for (int i = 4; i < 8; i++) sr = a[i].tick(sr);
      l[n] = sl; r[n] = sr;
    }
  }
};

// Dattorro plate: 4 input diffusers + figure-eight tank (2 allpasses + 2 delays per side).
struct Delay {
  float* buf; int len, idx = 0;
  inline float read(int d) const { int i = idx - d; if (i < 0) i += len; return buf[i]; }
  inline void write(float x) { buf[idx] = x; if (++idx >= len) idx = 0; }
};
struct Plate {
  Allpass in[4], tankAp[4];
  Delay tankDl[4];
  float lp1 = 0, lp2 = 0, decay = 0.5f, damp = 0.3f;
  bool ok = true;
  static int samples(float sec) { return (int)(sec * kSampleRate) + 1; }
  void init(bool psram) {
    const float inT[4] = {0.004762f, 0.003583f, 0.012721f, 0.009297f};
    const float apT[4] = {0.022562f, 0.060408f, 0.030454f, 0.089138f};
    const float dlT[4] = {0.149433f, 0.124830f, 0.141519f, 0.106145f};
    for (int i = 0; i < 4; i++) {
      in[i].len = samples(inT[i]); in[i].buf = allocBuf(in[i].len, psram); in[i].g = i < 2 ? 0.75f : 0.625f;
      tankAp[i].len = samples(apT[i]); tankAp[i].buf = allocBuf(tankAp[i].len, psram); tankAp[i].g = 0.6f;
      tankDl[i].len = samples(dlT[i]); tankDl[i].buf = allocBuf(tankDl[i].len, psram);
      ok &= in[i].buf && tankAp[i].buf && tankDl[i].buf;
    }
  }
  void block(const float* inBuf, float* l, float* r) {
    for (int n = 0; n < kBlock; n++) {
      float x = inBuf[n];
      for (int i = 0; i < 4; i++) x = in[i].tick(x);
      float a = x + tankDl[3].read(tankDl[3].len - 1) * decay;
      a = tankAp[0].tick(a);
      tankDl[0].write(a);
      lp1 += (1.0f - damp) * (tankDl[0].read(tankDl[0].len - 1) - lp1);
      float a2 = tankAp[1].tick(lp1 * decay);
      tankDl[1].write(a2);
      float b = x + tankDl[1].read(tankDl[1].len - 1) * decay;
      b = tankAp[2].tick(b);
      tankDl[2].write(b);
      lp2 += (1.0f - damp) * (tankDl[2].read(tankDl[2].len - 1) - lp2);
      float b2 = tankAp[3].tick(lp2 * decay);
      tankDl[3].write(b2);
      l[n] = tankDl[0].read(266) + tankDl[0].read(2974) - tankAp[1].buf[1913 % tankAp[1].len] + tankDl[1].read(1996);
      r[n] = tankDl[2].read(353) + tankDl[2].read(3627) - tankAp[3].buf[1228 % tankAp[3].len] + tankDl[3].read(2673);
    }
  }
};

static volatile float gSink;

static float cyclesPerSample(const std::function<void()>& run) {
  run();  // warm caches
  uint32_t t0 = esp_cpu_get_cycle_count();
  for (int b = 0; b < kBlocks; b++) run();
  uint32_t t1 = esp_cpu_get_cycle_count();
  return (float)(t1 - t0) / (float)(kBlocks * kBlock);
}

static void report(const char* name, float cps) {
  const float budget = 240e6f / kSampleRate;
  Serial.printf("%-34s %8.1f cyc/sample  (%5.1f%% of one core @48k)\n", name, cps, 100.0f * cps / budget);
}

void setup() {
  Serial.begin(115200);
  delay(2500);
  for (int i = 0; i <= kTableLen; i++) {
    float ph = (float)(i % kTableLen) / kTableLen;
    gTables[0][i] = (int16_t)(32000.0f * sinf(6.2831853f * ph));
    gTables[1][i] = (int16_t)(32000.0f * (2.0f * ph - 1.0f));
  }
  for (int i = 0; i <= 512; i++) gSine512[i] = sinf(6.2831853f * (float)(i % 512) / 512.0f);
  for (int i = 0; i < kBlock; i++) gIn[i] = (float)((i * 7919) % 200 - 100) / 100.0f;

  Serial.printf("\n=== Obsidian DSP bench: ESP32-S3 %u MHz, block %d, PSRAM %u KB free ===\n",
                (unsigned)getCpuFrequencyMhz(), kBlock, (unsigned)(ESP.getFreePsram() / 1024));

  WtOsc wt; wt.setFreq(220.0f);
  report("Wavetable osc (2-frame xfade)", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += wt.tick(); gSink = s; }));

  DpwSaw saw; saw.setFreq(220.0f);
  report("DPW saw (1 voice of NanoSaw)", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += saw.tick(); gSink = s; }));

  NanoSaw ns; ns.setFreq(220.0f, 0.5f);
  report("NanoSaw (7 DPW saws)", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += ns.tick(); gSink = s; }));

  Ladder lad; lad.setCutoff(1200.0f);
  report("Ladder 4-pole + clip", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += lad.tick(gIn[n]); gSink = s; }));

  Biquad bq;
  report("Biquad (-D filters, EQ)", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += bq.tick(gIn[n]); gSink = s; }));

  Env env;
  report("Envelope (rational curve, 1 div)", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += env.tick(); gSink = s; }));

  Lfo lfo;
  report("LFO sine (512 table)", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += lfo.tick(); gSink = s; }));

  Voice v; v.init(220.0f);
  float voiceCps = cyclesPerSample([&] { memset(gOut, 0, sizeof(gOut)); v.block(gOut); gSink = gOut[3]; });
  report("Voice: 3 WT osc + 2 ladder + 2 env", voiceCps);

  FastLadder flad; flad.setCutoff(1200.0f);
  report("Ladder 4-pole + clip (compares)", cyclesPerSample([&] { float s = 0; for (int n = 0; n < kBlock; n++) s += flad.tick(gIn[n]); gSink = s; }));
  FastVoice fv; fv.init(220.0f);
  float fastCps = cyclesPerSample([&] { memset(gOut, 0, sizeof(gOut)); fv.block(gOut); gSink = gOut[3]; });
  report("Voice optimized (block-rate env)", fastCps);

  Freeverb fvInt; fvInt.init(false);
  float fvIntCps = fvInt.ok ? cyclesPerSample([&] { fvInt.block(gIn, gOut, gOutR); gSink = gOut[1]; }) : -1;
  if (fvInt.ok) report("Freeverb 16 combs (internal RAM)", fvIntCps);
  Freeverb fvPs; fvPs.init(true);
  float fvPsCps = fvPs.ok ? cyclesPerSample([&] { fvPs.block(gIn, gOut, gOutR); gSink = gOut[1]; }) : -1;
  if (fvPs.ok) report("Freeverb 16 combs (PSRAM)", fvPsCps);

  Plate plInt; plInt.init(false);
  float plIntCps = plInt.ok ? cyclesPerSample([&] { plInt.block(gIn, gOut, gOutR); gSink = gOut[1]; }) : -1;
  if (plInt.ok) report("Dattorro plate (internal RAM)", plIntCps);
  else Serial.println("Dattorro plate (internal RAM): not enough internal RAM");
  Plate plPs; plPs.init(true);
  float plPsCps = plPs.ok ? cyclesPerSample([&] { plPs.block(gIn, gOut, gOutR); gSink = gOut[1]; }) : -1;
  if (plPs.ok) report("Dattorro plate (PSRAM)", plPsCps);

  const float budget = 240e6f / kSampleRate;
  float fxCps = plPsCps > 0 ? plPsCps : fvPsCps;
  Serial.printf("\nVoices per core with no FX: %.1f (optimized %.1f)\n", budget / voiceCps, budget / fastCps);
  Serial.printf("Voices on one core sharing it with the plate: %.1f (optimized %.1f)\n",
                (budget - fxCps) / voiceCps, (budget - fxCps) / fastCps);
  Serial.printf("Both cores, plate on core 0, ~70%% usable budget: %.1f optimized voices\n",
                (0.7f * budget * 2 - fxCps) / fastCps);
  Serial.println("=== done ===");
}

void loop() { delay(1000); }
