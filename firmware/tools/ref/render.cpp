// render <content.bin> <preset name> <out.raw> [fx] [stereo]: plays the reference sequence (seq.h; kFxSeq with "fx")
// through the port's engine on the host and writes the mono pre-volume mix (renderVoices + multiFx + plate) as 48 kHz
// float32, the same point cmpref compares against an NS2 recording; "stereo" writes L/R interleaved instead.
// Build with build.sh.
#include "synth.h"
#include "content.h"
#include <stdlib.h>
#include <vector>
#include "seq.h"
namespace syn { WaveBank* gBank; }
using namespace syn;

// Points content.h at an image read from a file instead of the flash partition.
static bool loadImage(const char* path, std::vector<uint8_t>& img) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  img.resize(16 << 20);
  img.resize(fread(img.data(), 1, img.size(), f));
  fclose(f);
  content::gBase = img.data();
  content::gHeader = (const content::Header*)img.data();
  const auto& h = *content::gHeader;
  const content::SampleEntry* s = content::at<content::SampleEntry>(h.sampleOff);
  for (uint32_t i = 0; i < h.sampleCount; i++)
    content::gSamples[i] = {content::at<int16_t>(s[i].offset), s[i].length, s[i].loopStart, s[i].loopEnd, s[i].rate / kSampleRate};
  const content::TableEntry* t = content::at<content::TableEntry>(h.tableOff);
  for (uint32_t i = 0; i < h.tableCount; i++)
    content::gTables[i] = {(int)t[i].frames, (const int16_t(*)[kMips][kTableLen + 1])(img.data() + t[i].offset), t[i].scale};
  const char* why = "";
  return content::resolvePaths(&why);
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: render <content.bin> <preset name> <out.raw> [fx] [stereo]\n"); return 2; }
  gBank = new WaveBank();
  gBank->build();
  static std::vector<uint8_t> img;
  if (!loadImage(argv[1], img)) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
  int idx = -1;
  for (int i = 0; i < content::presetCount(); i++) if (!strcmp(content::preset(i).name, argv[2])) idx = i;
  if (idx < 0) { fprintf(stderr, "no preset %s\n", argv[2]); return 1; }
  Engine* e = new Engine();
  e->init();
  static Patch p;
  content::loadPreset(idx, p);
  e->setPatch(p);
  std::vector<float> out;
  float L[kBlock], R[kBlock], wl[kBlock], wr[kBlock], rin[kBlock];
  bool fx = false, stereo = false;
  for (int i = 4; i < argc; i++) {
    fx |= !strcmp(argv[i], "fx");
    stereo |= !strcmp(argv[i], "stereo");
  }
  const RefEvent* seq = fx ? kFxSeq : kRefSeq;
  const size_t count = fx ? kFxSeqCount : kRefSeqCount;
  const int blocks = (int)(22.0f * kSampleRate / kBlock);
  for (int b = 0; b < blocks; b++) {
    const double t0 = b * kBlock / kSampleRate, t1 = (b + 1) * kBlock / kSampleRate;
    for (size_t i = 0; i < count; i++) {
      const RefEvent& v = seq[i];
      if (v.t >= t0 && v.t < t1) e->noteOn(v.note, v.vel);
      if (v.t + v.dur >= t0 && v.t + v.dur < t1) e->noteOff(v.note);
    }
    e->renderVoices(L, R);
    e->multiFx(L, R);
    e->reverbInput(L, R, rin);  // as the audio task feeds the reverb
    const float wet = e->reverbReady() ? e->reverbMix() : 0.0f, dry = fminf(1.0f, 2.0f * (1.0f - wet));
    if (wet > 0.0f) e->reverb(rin, wl, wr);
    else for (int n = 0; n < kBlock; n++) wl[n] = wr[n] = 0.0f;
    for (int n = 0; n < kBlock; n++) {
      const float l = L[n] * dry + wl[n] * wet, r = R[n] * dry + wr[n] * wet;
      if (stereo) { out.push_back(l); out.push_back(r); }
      else out.push_back(0.5f * (l + r));
    }
  }
  FILE* o = fopen(argv[3], "wb");
  fwrite(out.data(), 4, out.size(), o);
  fclose(o);
}
