// Obsidian-style synth on ESP32-S3 + PCM5102A over I2S, played over USB-MIDI.
// Voices render on core 1; the reverb runs on core 0, one block behind.
//
// The board option "USB Mode" selects how MIDI comes in on the native USB port:
//   USB-OTG (TinyUSB)      -> the board is a USB-MIDI device for a computer (Serial over the same port).
//   Hardware CDC and JTAG  -> the board is a USB host for a class-compliant controller plugged into
//                             the native port; Serial goes to UART0 (the COM / USB-UART port).
// In both modes a BLE-MIDI controller (e.g. Korg nanoKEY Studio on batteries) is found and connected over Bluetooth.

#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include "synth.h"
#include "wt_orchestral.h"
#include "content.h"
#include "ble_midi_central.h"

#if ARDUINO_USB_MODE
#define MIDI_USB_HOST 1
#include "usb_midi_host.h"
#else
#define MIDI_USB_HOST 0
#include "USB.h"
#include "USBMIDI.h"
SET_USB_MIDI_DEVICE_NAME("Obsidian ESP32")
#endif

static constexpr int kPinBck = 17;
static constexpr int kPinLck = 16;
static constexpr int kPinDin = 15;

namespace syn { WaveBank* gBank = nullptr; }
static const syn::Wavetable kOrchestral = {kOrchestralFrames, kOrchestralData, kOrchestralScale};

static I2SClass gI2s;
#if !MIDI_USB_HOST
static USBMIDI gMidi;
#endif
static syn::Engine* gEngine = nullptr;
static portMUX_TYPE gLock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool gDemo = true;
static volatile bool gMonitor = false;
static volatile uint32_t gVoiceCycles = 0, gReverbCycles = 0, gShed = 0;
static volatile int gPeak = 0;  // largest output sample since the last "stat"
static volatile uint32_t gMaxBusy = 0;  // longest audio-task block before its I2S write, in cycles
// "cap": the next second of output, 16-bit stereo, kept in PSRAM until "capdump" sends it.
static constexpr int kCapBlocks = (int)(syn::kSampleRate / syn::kBlock);
static int16_t* gCap = nullptr;
static volatile int gCapBlocks = -1;
// When the voices' load, averaged over the last ~16 blocks, passes this share of the block period,
// the oldest voice is dropped; single heavy blocks (note-ons) do not count.
static constexpr float kShedLoad = 0.9f;
static constexpr float kShedAverage = 1.0f / 16.0f;

// Reverb hand-off between cores: core 1 publishes a dry block, core 0 returns its wet block.
static float gRevIn[syn::kBlock], gWetL[syn::kBlock], gWetR[syn::kBlock];
static SemaphoreHandle_t gRevStart, gRevDone;

// Demo: Am - F - C - G pads with a simple arpeggio on top, 100 BPM. Stops on the first MIDI note.
struct Demo {
  static constexpr int kChords[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
  uint32_t samples = 0;
  int step = -1;
  int heldChord[3] = {-1, -1, -1};
  int heldArp = -1;

  void tick(syn::Engine& e) {
    const uint32_t stepLen = (uint32_t)(syn::kSampleRate * 60.0f / 100.0f / 4.0f);
    samples += syn::kBlock;
    if (samples < stepLen && step >= 0) return;
    samples = 0;
    step = (step + 1) % 64;
    int chord = step / 16;
    if (step % 16 == 0) {
      for (int& n : heldChord) if (n >= 0) e.noteOff(n);
      for (int i = 0; i < 3; i++) { heldChord[i] = kChords[chord][i] - 12; e.noteOn(heldChord[i], 70); }
    }
    if (heldArp >= 0) e.noteOff(heldArp);
    static const int pattern[8] = {0, 1, 2, 1, 0, 2, 1, 2};
    heldArp = kChords[chord][pattern[step % 8]] + 12;
    e.noteOn(heldArp, step % 4 == 0 ? 110 : 85);
  }

  void stop(syn::Engine& e) {
    for (int& n : heldChord) { if (n >= 0) e.noteOff(n); n = -1; }
    if (heldArp >= 0) e.noteOff(heldArp);
    heldArp = -1;
    step = -1;
  }
};
constexpr int Demo::kChords[4][3];
static Demo gDemoSeq;

static void reverbTask(void*) {
  for (;;) {
    xSemaphoreTake(gRevStart, portMAX_DELAY);
    uint32_t t0 = esp_cpu_get_cycle_count();
    if (gEngine->reverbReady() && gEngine->reverbMix() > 0.0f) {
      gEngine->reverb(gRevIn, gWetL, gWetR);
    } else {
      memset(gWetL, 0, sizeof(gWetL));
      memset(gWetR, 0, sizeof(gWetR));
    }
    gReverbCycles = esp_cpu_get_cycle_count() - t0;
    xSemaphoreGive(gRevDone);
  }
}

static void audioTask(void*) {
  static int16_t out[syn::kBlock * 2];
  static float dryL[syn::kBlock], dryR[syn::kBlock], wetL[syn::kBlock], wetR[syn::kBlock];
  bool demoWasOn = true;
  float load = 0;  // voice cycles / block budget, averaged
  for (;;) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    portENTER_CRITICAL(&gLock);
    if (gDemo) gDemoSeq.tick(*gEngine);
    else if (demoWasOn) gDemoSeq.stop(*gEngine);
    demoWasOn = gDemo;
    portEXIT_CRITICAL(&gLock);
    gEngine->renderVoices(dryL, dryR);
    gEngine->multiFx(dryL, dryR);
    gVoiceCycles = esp_cpu_get_cycle_count() - t0;
    load += (gVoiceCycles / (240e6f / syn::kSampleRate * syn::kBlock) - load) * kShedAverage;
    if (load > kShedLoad) {
      portENTER_CRITICAL(&gLock);
      gEngine->shedOldest();
      portEXIT_CRITICAL(&gLock);
      gShed = gShed + 1;
      load = kShedLoad - 0.1f;  // give the lighter mix a few blocks before judging again
    }

    // Collect the wet block of the previous cycle and hand this dry block to core 0.
    xSemaphoreTake(gRevDone, portMAX_DELAY);
    memcpy(wetL, gWetL, sizeof(wetL));
    memcpy(wetR, gWetR, sizeof(wetR));
    gEngine->reverbInput(dryL, dryR, gRevIn);
    xSemaphoreGive(gRevStart);

    gEngine->mixOut(dryL, dryR, wetL, wetR, out);
    const uint32_t busy = esp_cpu_get_cycle_count() - t0;
    if (busy > gMaxBusy) gMaxBusy = busy;
    if (gCapBlocks >= 0 && gCapBlocks < kCapBlocks) {
      memcpy(gCap + gCapBlocks * syn::kBlock * 2, out, sizeof(out));
      gCapBlocks = gCapBlocks + 1;
    }
    int peak = gPeak;
    for (int n = 0; n < syn::kBlock * 2; n++) peak = abs(out[n]) > peak ? abs(out[n]) : peak;
    gPeak = peak;
    gI2s.write((uint8_t*)out, sizeof(out));
  }
}

static float cc01(uint8_t v) { return (float)v / 127.0f; }

// Knob-assignable parameters. Values arrive normalized 0..1 from a CC or the serial console.
enum Param { kCut, kRes, kEnvAmt, kRev, kVol, kGlide, kAtk, kDec, kSus, kRel, kFdec, kDet, kLfoRate, kLfoAmt, kParamCount };
static const char* const kParamNames[kParamCount] = {"cut", "res", "env", "rev", "vol", "glide", "atk",
                                                     "dec", "sus", "rel", "fdec", "det", "lforate", "lfoamt"};
static int8_t gCcMap[128];
static volatile int gLearn = -1;
static Preferences gPrefs;

// The last value of every knob-assignable parameter (-1 before it moves), so a new preset starts
// from where the controls are.
static float gParamValue[kParamCount];

static void setParam(int id, float x) {
  syn::Patch& p = gEngine->patch;
  x = constrain(x, 0.0f, 1.0f);
  gParamValue[id] = x;
  switch (id) {
    case kCut: p.cutoff = x; break;
    case kRes: p.resonance = x; break;
    case kEnvAmt: p.filterEnvAmt = x; break;
    case kRev: p.reverbMix = 0.5f * x; break;  // up to Mix 0.5: the dry stays at full level (NS2 fades it above)
    case kVol: p.volume = x * x; break;  // audio taper
    case kGlide: p.glide = 2.0f * x * x; break;
    case kAtk: p.ampEnv.attack = 0.001f + 5.0f * x * x * x; break;
    case kDec: p.ampEnv.decay = 0.01f + 10.0f * x * x * x; break;
    case kSus: p.ampEnv.sustain = x; break;
    case kRel: p.ampEnv.release = 0.01f + 10.0f * x * x * x; break;
    case kFdec: p.filtEnv.decay = 0.01f + 10.0f * x * x * x; break;
    case kDet: p.osc[1].fine = 0.5f * x; break;
    // LFO 2: the filter wobble of Obsidian's layout (LFO 2 -> Filter 1+2 Cutoff in 436 presets).
    case kLfoRate: p.lfo[1].rate = 0.05f * powf(400.0f, x); break;  // 0.05..20 Hz
    case kLfoAmt: p.lfo[1].ampl = x; break;
  }
}

// Presets, selected by Program Change, the Korg pads or the 'preset' command: 0..7 are written
// here, the content image's presets (content.h) follow.
static constexpr int kBuiltIns = 8;
static constexpr const char* kBootPreset = "RhodesMK2Raw";  // loaded at boot when the content image has it
static const char* const kPresetNames[kBuiltIns] = {"Poly",  "Pad",   "Bass",  "Lead",
                                                    "Pluck", "Organ", "Brass", "Abandoned Factory"};
// Pad categories (content2bin.py's PAD_ORDER), one per Korg pad.
static constexpr int kKorgPads = 8;
static const char* const kCategoryNames[kKorgPads] = {"Bass", "Lead", "Pad", "Pluck", "Brass", "Other", "Drums", "Keys"};
static const uint8_t kBuiltInCategory[kBuiltIns] = {5, 2, 0, 1, 3, 7, 4, 2};

static int presetCount() { return kBuiltIns + content::presetCount(); }
static const char* presetName(int i) { return i < kBuiltIns ? kPresetNames[i] : content::preset(i - kBuiltIns).name; }
static int presetCategory(int i) { return i < kBuiltIns ? kBuiltInCategory[i] : content::preset(i - kBuiltIns).category; }
static volatile int gPresetLoaded = -1;

// Fills `p` with preset i. The patch is several KB, so callers pass static storage rather than
// the stack of the MIDI task that loads it.
static void makePreset(int i, syn::Patch& p) {
  using namespace syn;
  new (&p) Patch();  // "Poly": the engine's default patch, built in place
  auto osc = [&](int n, Wave w, int trn, float fine, float lev) {
    p.osc[n].enabled = lev > 0.0f; p.osc[n].wave = w; p.osc[n].transpose = trn;
    p.osc[n].fine = fine; p.osc[n].level = lev;
  };
  auto env = [](EnvPatch& e, float a, float d, float s, float r) {
    e.attack = a; e.decay = d; e.sustain = s; e.release = r;
  };
  switch (i) {
    case 1:  // Pad
      osc(0, kSaw, 0, 0.0f, 0.5f); osc(1, kSaw, 0, 0.15f, 0.5f); osc(2, kTri, 12, 0.0f, 0.3f);
      env(p.ampEnv, 0.8f, 1.0f, 0.8f, 2.0f); env(p.filtEnv, 1.2f, 2.0f, 0.5f, 2.0f);
      p.cutoff = 0.4f; p.resonance = 0.2f; p.filterEnvAmt = 0.2f; p.reverbMix = 0.45f; p.reverbDecay = 0.75f;
      break;
    case 2:  // Bass
      osc(0, kSquare, -12, 0.0f, 0.6f); osc(1, kSaw, -12, 0.05f, 0.5f); osc(2, kSine, -24, 0.0f, 0.4f);
      env(p.ampEnv, 0.002f, 0.3f, 0.8f, 0.08f); env(p.filtEnv, 0.002f, 0.25f, 0.1f, 0.1f);
      p.cutoff = 0.22f; p.resonance = 0.5f; p.filterEnvAmt = 0.45f; p.reverbMix = 0.05f;
      break;
    case 3:  // Lead
      osc(0, kSaw, 0, 0.0f, 0.6f); osc(1, kSquare, 0, 0.12f, 0.4f); osc(2, kSaw, 12, 0.0f, 0.2f);
      env(p.ampEnv, 0.005f, 0.5f, 0.85f, 0.2f); env(p.filtEnv, 0.01f, 0.6f, 0.4f, 0.3f);
      p.cutoff = 0.5f; p.resonance = 0.4f; p.filterEnvAmt = 0.25f; p.glide = 0.06f; p.reverbMix = 0.25f;
      break;
    case 4:  // Pluck
      osc(0, kSaw, 0, 0.0f, 0.6f); osc(1, kTri, 12, 0.0f, 0.4f); osc(2, kSaw, 0, 0.0f, 0.0f);
      env(p.ampEnv, 0.002f, 0.6f, 0.0f, 0.4f); env(p.filtEnv, 0.001f, 0.2f, 0.0f, 0.2f);
      p.cutoff = 0.2f; p.resonance = 0.3f; p.filterEnvAmt = 0.5f; p.reverbMix = 0.3f;
      break;
    case 5:  // Organ
      osc(0, kSine, 0, 0.0f, 0.6f); osc(1, kSine, 12, 0.0f, 0.4f); osc(2, kSine, 19, 0.0f, 0.25f);
      env(p.ampEnv, 0.005f, 0.1f, 1.0f, 0.05f);
      p.cutoff = 0.9f; p.resonance = 0.0f; p.filterEnvAmt = 0.0f; p.reverbMix = 0.2f;
      break;
    case 6:  // Brass
      osc(0, kSaw, 0, 0.0f, 0.55f); osc(1, kSaw, 0, 0.1f, 0.55f); osc(2, kSquare, -12, 0.0f, 0.2f);
      env(p.ampEnv, 0.04f, 0.4f, 0.8f, 0.25f); env(p.filtEnv, 0.08f, 0.5f, 0.4f, 0.3f);
      p.cutoff = 0.3f; p.resonance = 0.15f; p.filterEnvAmt = 0.4f; p.reverbMix = 0.2f;
      break;
    case 7: {  // NS2 factory preset Obsidian/Abandoned Factory.obs (approximation, see 10-porting-notes.md)
      osc(0, kSaw, 7, 0.0f, 0.0f); osc(1, kSine, -24, 0.0f, 2.0f); osc(2, kSaw, -24, 0.0f, 0.727474f);
      p.osc[0].enabled = true;  // Lev 0, but it ring-modulates Osc 2
      p.osc[0].type = kOscTable; p.osc[0].table = &kOrchestral; p.osc[0].position = 0.5f;
      p.osc[1].ring = 1.0f;
      p.osc[2].type = kOscTable; p.osc[2].table = &kOrchestral; p.osc[2].position = 0.5f;
      p.filterType = kFilterLP12D; p.cutoff = 0.254664f; p.resonance = 0.0f; p.keyTrack = 0.0f;
      p.filterGain = 1.3386f;
      p.filterEnvAmt = 1.0f;  // C00 Filter Env -> Filter 1+2 Cutoff, Dept 100
      env(p.ampEnv, 0.634413f, 2.5f, 1.0f, 4.84776f);
      p.ampEnv.ac = 2.96465f; p.ampEnv.rc = 1.4834f; p.ampEnv.level = 0.60258f;
      p.ampEnv.vLev = 0.4f; p.ampEnv.kCtr = 24; p.ampEnv.kLev = 0.4f;
      env(p.filtEnv, 1.1705f, 2.5f, 0.5f, 6.58926f);
      p.filtEnv.ac = 2.61438f; p.filtEnv.rc = 1.34478f; p.filtEnv.level = 0.667335f;
      p.lfo[2] = {36.631f, 0.326667f};
      p.lfo[3] = {0.153652f, 0.825501f};
      p.lfo[4] = {0.280185f, 0.301f};
      p.mod[1] = {srcLfo(2), kSrcNone, kDstCutoff, 0, 0.03f};        // C04 LFO 3 -> Filter 1+2 Cutoff
      p.mod[2] = {srcLfo(3), kSrcNone, kDstCutoff, 0, 0.30f};        // C06 LFO 4 -> Filter 1+2 Cutoff
      p.mod[3] = {srcLfo(4), kSrcNone, kDstWtPos, 0, 0.80f};         // C07 LFO 5 -> Osc 1 WT Position
      p.mod[4] = {srcLfo(4), kSrcNone, kDstWtPos, 2, -0.53f};        // C09 LFO 5 -> Osc 3 WT Position
      p.mod[5] = {srcLfo(4), kSrcNone, kDstGlobalCutoff, 0, 1.0f};   // C10 LFO 5 -> Global Filter Cutoff
      p.comb.enabled = true; p.comb.negative = true; p.comb.cutoff = 0.258999f; p.comb.q = 0.0f;
      p.comb.mix = 1.0f;
      p.unison = 2; p.unisonDetune = 0.122666f; p.unisonStereo = 0.867333f;
      p.reverbMix = 0.0f;  // FX mixes are 0 (knobs 3/4 at zero)
      break;
    }
    default:
      content::loadPreset(i - kBuiltIns, p);
      break;
  }
}

// Caller holds gLock.
static void loadPreset(int i) {
  static syn::Patch built;
  makePreset(i, built);
  gEngine->setPatch(built);
  for (int id = 0; id < kParamCount; id++) if (gParamValue[id] >= 0.0f) setParam(id, gParamValue[id]);
  gPresetLoaded = i;
}

static int paramByName(const char* s) {
  for (int i = 0; i < kParamCount; i++) if (!strcmp(s, kParamNames[i])) return i;
  return -1;
}

// Each Korg pad is a preset category: pressing it loads the category's last preset, pressing
// it again steps to the next one. The lists are built at boot from the presets' categories.
static constexpr int kMaxPresets = 1024;
static uint16_t gPadList[kMaxPresets];
static int gPadStart[kKorgPads + 1];
static int gPadSlot[kKorgPads];
static int gLastPad = -1;

static void buildPadLists() {
  int n = 0;
  for (int pad = 0; pad < kKorgPads; pad++) {
    gPadStart[pad] = n;
    for (int i = 0; i < presetCount() && n < kMaxPresets; i++)
      if (presetCategory(i) == pad) gPadList[n++] = (uint16_t)i;
  }
  gPadStart[kKorgPads] = n;
}

// Returns the preset a pad selects, or -1 for an empty category. Caller holds gLock.
static int padPreset(int pad) {
  const int count = gPadStart[pad + 1] - gPadStart[pad];
  if (count == 0) return -1;
  if (pad == gLastPad) gPadSlot[pad] = (gPadSlot[pad] + 1) % count;
  gLastPad = pad;
  return gPadList[gPadStart[pad] + gPadSlot[pad]];
}

// Korg nanoKEY Studio factory layouts (measured). The knob CCs overlap between USB and Bluetooth,
// so they are matched by channel instead of going into the channel-less CC map.
struct KorgLayout {
  uint8_t channel;    // 0-based
  uint8_t knobCc;     // knobs 1..8 send knobCc..knobCc+7
  bool padsAreNotes;  // pads 1..8 send notes padFirst.. (else CCs padFirst.., 127 press / 0 release)
  uint8_t padChannel, padFirst;
};
static const KorgLayout kKorgLayouts[] = {
    {15, 16, true, 1, 64},  // USB: knobs ch16 CC16-23, pads ch2 notes 64-71
    {0, 20, false, 0, 95},  // Bluetooth: knobs ch1 CC20-27, pads ch1 CC95-102
};
// Each knob drives the same parameter in every preset, in the order of Obsidian's factory macros.
// The preset's macros stay at its saved positions (Autm).
static const int8_t kKorgKnobParams[8] = {kCut, kRes, kAtk, kRel, kLfoRate, kLfoAmt, kRev, kVol};

// Returns the Korg pad pressed (0..7, -2 for its release), or -1 if the message is not a pad.
static int korgPad(uint8_t ch, uint8_t status, uint8_t d1, uint8_t d2) {
  for (const KorgLayout& k : kKorgLayouts) {
    if (ch != k.padChannel || d1 < k.padFirst || d1 >= k.padFirst + kKorgPads) continue;
    bool press = k.padsAreNotes ? status == 0x90 && d2 > 0 : status == 0xb0 && d2 > 0;
    bool release = k.padsAreNotes ? status == 0x80 || (status == 0x90 && d2 == 0) : status == 0xb0 && d2 == 0;
    if (press) return d1 - k.padFirst;
    if (release) return -2;
  }
  return -1;
}

// Returns the Korg knob (0..7) a CC comes from, or -1.
static int korgKnob(uint8_t ch, uint8_t cc) {
  for (const KorgLayout& k : kKorgLayouts)
    if (ch == k.channel && cc >= k.knobCc && cc < k.knobCc + 8) return cc - k.knobCc;
  return -1;
}

// Caller holds gLock.
static void korgKnobMoved(int knob, float x) {
  setParam(kKorgKnobParams[knob], x);
}

static void loadCcMap() {
  memset(gCcMap, -1, sizeof(gCcMap));
  // Defaults: GM sound controllers plus the usual cutoff/resonance CCs.
  gCcMap[74] = kCut; gCcMap[71] = kRes; gCcMap[1] = kEnvAmt; gCcMap[91] = kRev; gCcMap[7] = kVol;
  gCcMap[5] = kGlide; gCcMap[73] = kAtk; gCcMap[75] = kDec; gCcMap[79] = kSus; gCcMap[72] = kRel;
  if (gPrefs.getBytesLength("ccmap") == sizeof(gCcMap)) gPrefs.getBytes("ccmap", gCcMap, sizeof(gCcMap));
}

static void saveCcMap() { gPrefs.putBytes("ccmap", gCcMap, sizeof(gCcMap)); }

// MIDI messages are applied under the same lock the audio task uses for the demo sequencer.
static void handleMidi(uint8_t b0, uint8_t b1, uint8_t b2) {
  uint8_t status = b0 & 0xf0;
  uint8_t d1 = b1, d2 = b2;
  if (gMonitor && b0 < 0xf0) Serial.printf("midi ch%d %02x %d %d\n", (b0 & 0x0f) + 1, status, d1, d2);
  int pad = korgPad(b0 & 0x0f, status, d1, d2);
  portENTER_CRITICAL(&gLock);
  if (pad != -1) {
    const int preset = pad >= 0 ? padPreset(pad) : -1;
    if (preset >= 0) loadPreset(preset);
    portEXIT_CRITICAL(&gLock);
    return;
  }
  switch (status) {
    case 0xc0: loadPreset(d1 % presetCount()); break;
    case 0x90:
      if (gDemo && d2 > 0) gDemo = false;
      gEngine->noteOn(d1, d2);
      break;
    case 0x80: gEngine->noteOff(d1); break;
    case 0xe0: gEngine->wheel = ((float)(((int)d2 << 7) | d1) - 8192.0f) / 8192.0f; break;
    case 0xb0:
      if (d1 == 64) gEngine->setSustain(d2 >= 64);
      else if (d1 == 123) gEngine->allOff();
      else if (gLearn >= 0) {
        for (auto& m : gCcMap) if (m == gLearn) m = -1;
        gCcMap[d1] = (int8_t)gLearn;
        gLearn = -2 - d1;  // tell loop() to save and report
      } else if (gCcMap[d1] >= 0) {
        setParam(gCcMap[d1], cc01(d2));
      } else if (korgKnob(b0 & 0x0f, d1) >= 0) {
        korgKnobMoved(korgKnob(b0 & 0x0f, d1), cc01(d2));
      }
      break;
  }
  portEXIT_CRITICAL(&gLock);
}

#if MIDI_USB_HOST
static void onHostPacket(const uint8_t p[4]) { handleMidi(p[1], p[2], p[3]); }
#endif

static void printHelp() {
  Serial.println(
      "USB-MIDI: notes, pitch bend (+-2 st), CC64 sustain, CC74 cutoff, CC71 resonance,\n"
      "          CC1 filter env amount, CC91 reverb, CC7 volume, CC5 glide, CC123 all off.\n"
      "Serial commands:\n"
      "  on <note> [vel] | off <note> | demo 0|1\n"
      "  <param> <0..1>   params: cut res env rev vol glide atk dec sus rel fdec det lforate lfoamt\n"
      "  wave <osc 0..2> <0 sine|1 tri|2 saw|3 square>\n"
      "  preset <n>       0..7 Poly Pad Bass Lead Pluck Organ Brass Factory, then the content image\n"
      "                   (also Program Change)\n"
      "  list [pad 1..8]  presets, all or of one pad   | find <text>  presets whose name has <text>\n"
      "  Korg pads 1..8:  Bass Lead Pad Pluck Brass Other Drums Keys; press again for the next preset\n"
      "                   Knobs keep their position across presets\n"
      "  learn <param>    next CC moved is assigned to <param> (saved in flash)\n"
      "  map | unmap <cc> list or remove CC assignments\n"
      "  mon 0|1          print incoming MIDI messages (USB and Bluetooth)\n"
      "  stat             CPU load per core, longest block and output peak since the last stat\n"
      "  cap | capdump    record the next second of output | send it (\"CAP <bytes>\" then 16-bit stereo)");
}

static void handleLine(char* line) {
  char* cmd = strtok(line, " \t");
  if (!cmd) return;
  char* a = strtok(nullptr, " \t");
  char* b = strtok(nullptr, " \t");
  float fa = a ? atof(a) : 0.0f;
  syn::Patch& p = gEngine->patch;
  portENTER_CRITICAL(&gLock);
  if (!strcmp(cmd, "on") && a) gEngine->noteOn(atoi(a), b ? atoi(b) : 100);
  else if (!strcmp(cmd, "off") && a) gEngine->noteOff(atoi(a));
  else if (!strcmp(cmd, "demo") && a) gDemo = atoi(a) != 0;
  else if (!strcmp(cmd, "mon") && a) gMonitor = atoi(a) != 0;
  else if (!strcmp(cmd, "preset") && a) loadPreset(constrain(atoi(a), 0, presetCount() - 1));
  else if (!strcmp(cmd, "wave") && a && b) p.osc[constrain(atoi(a), 0, 2)].wave = (syn::Wave)constrain(atoi(b), 0, 3);
  else if (paramByName(cmd) >= 0 && a) setParam(paramByName(cmd), fa);
  portEXIT_CRITICAL(&gLock);
  if (!strcmp(cmd, "learn") && a) {
    int id = paramByName(a);
    if (id < 0) Serial.println("unknown parameter");
    else { gLearn = id; Serial.printf("move a knob to assign it to '%s'\n", a); }
  } else if (!strcmp(cmd, "map")) {
    for (int c = 0; c < 128; c++) if (gCcMap[c] >= 0) Serial.printf("  CC%-3d -> %s\n", c, kParamNames[gCcMap[c]]);
  } else if (!strcmp(cmd, "unmap") && a) {
    gCcMap[constrain(atoi(a), 0, 127)] = -1;
    saveCcMap();
  }
  if (!strcmp(cmd, "list") || (!strcmp(cmd, "find") && a)) {
    const int pad = !strcmp(cmd, "list") && a ? constrain(atoi(a), 1, kKorgPads) - 1 : -1;
    for (int i = 0; i < presetCount(); i++) {
      if (pad >= 0 && presetCategory(i) != pad) continue;
      if (!strcmp(cmd, "find") && !strcasestr(presetName(i), a)) continue;
      Serial.printf("  %3d %-6s %s\n", i, kCategoryNames[presetCategory(i)], presetName(i));
    }
  }
  if (!strcmp(cmd, "stat")) {
    float budget = 240e6f / syn::kSampleRate * syn::kBlock;
    Serial.printf("voices active %d (%u shed) | core 1 voices %.1f%% | core 0 reverb %.1f%% | longest block %.0f%% | peak %.1f dBFS | BLE %s, %u packets\n",
                  gEngine->activeVoices(), (unsigned)gShed, 100.0f * gVoiceCycles / budget, 100.0f * gReverbCycles / budget,
                  100.0f * gMaxBusy / budget,
                  20.0f * log10f((gPeak + 1) / 32768.0f), blemidi::gState.connected ? "connected" : "scanning",
                  (unsigned)blemidi::gState.packets);
    gPeak = 0;
    gMaxBusy = 0;
  } else if (!strcmp(cmd, "cap")) {
    if (!gCap) gCap = (int16_t*)heap_caps_malloc(kCapBlocks * syn::kBlock * 4, MALLOC_CAP_SPIRAM);
    if (gCap) gCapBlocks = 0;
    Serial.println(gCap ? "capturing 1 s" : "out of PSRAM");
  } else if (!strcmp(cmd, "capdump")) {
    if (gCapBlocks < kCapBlocks) {
      Serial.println("no complete capture");
    } else {
      Serial.printf("CAP %u\n", (unsigned)(kCapBlocks * syn::kBlock * 4));
      Serial.write((const uint8_t*)gCap, kCapBlocks * syn::kBlock * 4);
    }
  } else if (!strcmp(cmd, "help")) {
    printHelp();
  }
}

void setup() {
  Serial.begin(115200);
#if !MIDI_USB_HOST
  gMidi.begin();
  USB.begin();
#endif
  delay(1500);
  Serial.printf("\nObsidian ESP32 synth (USB-MIDI %s)\n", MIDI_USB_HOST ? "host" : "device");
  gPrefs.begin("obsidian", false);

  // The wave bank (~90 KB, read through the cache like the flash wavetables) goes to PSRAM so the
  // internal RAM keeps room for the voices and the Bluetooth stack.
  syn::gBank = (syn::WaveBank*)heap_caps_malloc(sizeof(syn::WaveBank), MALLOC_CAP_SPIRAM);
  gEngine = (syn::Engine*)heap_caps_malloc(sizeof(syn::Engine), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!syn::gBank || !gEngine) {
    Serial.println(!syn::gBank ? "out of PSRAM" : "out of internal RAM");
    return;
  }
  uint32_t t0 = millis();
  syn::gBank->build();
  new (gEngine) syn::Engine();
  gEngine->init();
  loadCcMap();
  const char* why = "";
  if (content::begin(&why) >= 0)
    Serial.printf("content: %d presets, %d wavetables, %d samples\n", content::presetCount(), content::tableCount(),
                  content::sampleCount());
  else
    Serial.printf("content: %s; only the %d built-in presets\n", why, kBuiltIns);
  for (float& v : gParamValue) v = -1.0f;
  buildPadLists();
  for (int i = 0; i < presetCount(); i++)
    if (!strcmp(presetName(i), kBootPreset)) { loadPreset(i); break; }
  Serial.printf("wavetables built in %u ms, reverb %s, free internal %u KB, free PSRAM %u KB\n",
                (unsigned)(millis() - t0), gEngine->plate.ok && gEngine->room.ok && gEngine->reverbPre.buf ? "ok" : "FAILED",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(ESP.getFreePsram() / 1024));

  gI2s.setPins(kPinBck, kPinLck, kPinDin);
  if (!gI2s.begin(I2S_MODE_STD, (uint32_t)syn::kSampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S init failed");
    return;
  }
  gRevStart = xSemaphoreCreateBinary();
  gRevDone = xSemaphoreCreateBinary();
  xSemaphoreGive(gRevDone);  // the first block mixes silence as its wet signal
  xTaskCreatePinnedToCore(reverbTask, "reverb", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 0);
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 1, nullptr, 1);
#if MIDI_USB_HOST
  if (!midihost::begin(onHostPacket, 0)) Serial.println("USB host init failed");
  else Serial.println("USB host ready: plug a MIDI controller into the native USB port");
#endif
  blemidi::begin(handleMidi, "Obsidian ESP32");
  Serial.println("BLE MIDI: scanning for a Bluetooth MIDI controller");
  Serial.println("audio on core 1, reverb on core 0 (BCK 17, LCK 16, DIN 15). Type 'help'.");
}

void loop() {
#if !MIDI_USB_HOST
  midiEventPacket_t pk;
  while (gMidi.readPacket(&pk)) handleMidi(pk.byte1, pk.byte2, pk.byte3);
#endif
  blemidi::poll();
  if (gLearn <= -2) {
    int cc = -2 - gLearn;
    gLearn = -1;
    saveCcMap();
    Serial.printf("CC%d -> %s (saved)\n", cc, kParamNames[gCcMap[cc]]);
  }
  if (gPresetLoaded >= 0) {
    Serial.printf("preset %d: %s\n", gPresetLoaded, presetName(gPresetLoaded));
    gPresetLoaded = -1;
  }

  static char line[96];
  static int len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = 0;
      handleLine(line);
      len = 0;
    } else if (len < (int)sizeof(line) - 1) {
      line[len++] = c;
    }
  }
  delay(1);
}
