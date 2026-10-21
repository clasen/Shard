// cmpref <ns2.raw|wav> <port.raw>: compares an NS2 recording of the reference sequence (seq.h) with the
// port's render. The takes are aligned by their energy envelopes; per note it prints the level against the
// global gain, the envelope at 10 ms..1.8 s (dB from the peak), octave bands in the sustain and the pitch.
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <complex>
#include <algorithm>
#include "seq.h"
typedef std::vector<float> V;
static V loadWav(const char* p) {  // 16-bit PCM mono/stereo, finds the data chunk
  FILE* f = fopen(p, "rb"); std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n); fclose(f);
  int ch = *(uint16_t*)&b[22]; size_t i = 12; V out;
  while (i + 8 < b.size()) { uint32_t sz = *(uint32_t*)&b[i + 4]; if (!memcmp(&b[i], "data", 4)) { const int16_t* s = (const int16_t*)&b[i + 8];
      for (size_t k = 0; k + ch <= sz / 2; k += ch) { float a = 0; for (int c = 0; c < ch; c++) a += s[k + c]; out.push_back(a / ch / 32768.0f); } break; } i += 8 + sz; }
  return out;
}
static V loadRaw(const char* p) { FILE* f = fopen(p, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f) / 4; fseek(f, 0, 0); V v(n); fread(v.data(), 4, n, f); fclose(f); return v; }
static double rms(const V& x, long a, long n) { double e = 0; for (long i = a; i < a + n && i < (long)x.size(); i++) if (i >= 0) e += x[i] * x[i]; return sqrt(e / n); }
static long onset(const V& x, double thr) { for (long i = 0; i + 48 < (long)x.size(); i += 48) if (rms(x, i, 48) > thr) return i; return -1; }
static void fft(std::vector<std::complex<double>>& a) {
  size_t n = a.size(); for (size_t i = 1, j = 0; i < n; i++) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
  for (size_t len = 2; len <= n; len <<= 1) { double ang = -2 * M_PI / len; std::complex<double> wl(cos(ang), sin(ang));
    for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t j = 0; j < len / 2; j++) { auto u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } } }
}
static const double kBands[] = {63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
static void bands(const V& x, long a, double* out) {  // octave-band energy in dB of a 0.34 s Hann window
  const size_t N = 16384; std::vector<std::complex<double>> c(N);
  for (size_t i = 0; i < N; i++) { long k = a + i; double w = 0.5 - 0.5 * cos(2 * M_PI * i / N); c[i] = (k >= 0 && k < (long)x.size() ? x[k] : 0) * w; }
  fft(c);
  for (int b = 0; b < 9; b++) { double lo = kBands[b] / sqrt(2), hi = kBands[b] * sqrt(2), e = 1e-20;
    for (size_t i = 1; i < N / 2; i++) { double f = i * 48000.0 / N; if (f >= lo && f < hi) e += std::norm(c[i]); } out[b] = 10 * log10(e); }
}
static double pitchHz(const V& x, long a, double expect) {  // autocorrelation peak near the expected period
  const long N = 8192; double best = -1; double bl = 0; long lo = (long)(48000 / (expect * 1.12)), hi = (long)(48000 / (expect / 1.12));
  std::vector<double> r(hi + 2);
  for (long l = lo - 1; l <= hi + 1; l++) { double s = 0; for (long i = 0; i < N; i++) { long k = a + i; if (k + l < (long)x.size() && k >= 0) s += x[k] * x[k + l]; } r[l - lo + 1 > 0 ? l - lo + 1 : 0] = s; if (l >= lo && l <= hi && s > best) { best = s; bl = l; } }
  long i = (long)bl - lo + 1; if (i <= 0 || i + 1 >= (long)r.size()) return 48000 / bl;
  double y0 = r[i - 1], y1 = r[i], y2 = r[i + 1], d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2); return 48000 / (bl + d);
}
int main(int argc, char** argv) {
  V ns = strstr(argv[1], ".raw") ? loadRaw(argv[1]) : loadWav(argv[1]), po = loadRaw(argv[2]);
  double noise = rms(ns, 0, 9600); long on = onset(ns, fmax(noise * 6, 1e-4)), op = onset(po, 1e-5);
  if (on < 0 || op < 0) { printf("no onset (ns2 %ld, port %ld)\n", on, op); return 1; }
  // fine alignment: cross-correlate 10 ms energy envelopes within +-60 ms
  long best = 0; double bc = -1;
  for (long d = -2880; d <= 2880; d += 48) { double c = 0; for (long t = 0; t < 48000 * 6; t += 480) c += rms(ns, on + t + d, 480) * rms(po, op + t, 480); if (c > bc) { bc = c; best = d; } }
  on += best;
  // One segment per onset time of the sequence; several notes at one time are a chord.
  struct Seg { double t; int note, vel; char l[16]; } seg[kRefSeqCount]; int nseg = 0;
  for (size_t i = 0; i < kRefSeqCount; i++) {
    const RefEvent& e = kRefSeq[i];
    if (nseg && seg[nseg - 1].t == e.t) { strcpy(seg[nseg - 1].l, "chord"); seg[nseg - 1].note = -1; continue; }
    seg[nseg] = {e.t, e.note, e.vel, ""};
    snprintf(seg[nseg].l, sizeof seg[nseg].l, "%s%d v%d", (const char*[]){"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}[e.note % 12], e.note / 12 - 1, e.vel);
    nseg++;
  }
  double gl[kRefSeqCount]; for (int s = 0; s < nseg; s++) { long a = seg[s].t * 48000; gl[s] = 20 * log10((rms(ns, on + a, 72000) + 1e-12) / (rms(po, op + a, 72000) + 1e-12)); }
  double sg[kRefSeqCount]; memcpy(sg, gl, sizeof sg); std::sort(sg, sg + nseg); const double G = sg[nseg / 2], g = pow(10, G / 20);
  printf("noise floor %.1f dBFS, offset %ld ms, global gain NS2/port %.1f dB\n", 20 * log10(noise + 1e-12), (on - op) / 48, G);
  printf("%-8s %6s | %-34s | %-44s | %6s\n", "note", "lvl dB", "envelope dB vs peak (ns2/port) at 10ms 100ms 0.5s 1.5s 1.8s", "octave bands ns2-port dB (63..16k), sustain", "cents");
  for (int s = 0; s < nseg; s++) {
    long a = seg[s].t * 48000; double pkN = 0, pkP = 0;
    for (long t = 0; t < 72000; t += 480) { pkN = fmax(pkN, rms(ns, on + a + t, 480)); pkP = fmax(pkP, rms(po, op + a + t, 480) * g); }
    printf("%-8s %+6.1f |", seg[s].l, gl[s] - G);
    for (double tt : {0.01, 0.1, 0.5, 1.5, 1.8}) { long k = tt * 48000; printf(" %4.0f/%-4.0f", 20 * log10(rms(ns, on + a + k, 480) / pkN + 1e-9), 20 * log10(rms(po, op + a + k, 480) * g / pkP + 1e-9)); }
    double bn[9], bp[9]; bands(ns, on + a + 4800, bn); bands(po, op + a + 4800, bp);
    double tn = 0, tp = 0; for (int b = 0; b < 9; b++) { tn += pow(10, bn[b] / 10); tp += pow(10, bp[b] / 10); }
    printf(" |"); for (int b = 0; b < 9; b++) { double d = (bn[b] - 10 * log10(tn)) - (bp[b] - 10 * log10(tp)); if (bn[b] - 10 * log10(tn) < -50 && bp[b] - 10 * log10(tp) < -50) printf("    ."); else printf(" %+4.0f", d); }
    if (seg[s].note >= 0) { double f0 = 440 * pow(2, (seg[s].note - 69) / 12.0), pn = pitchHz(ns, on + a + 4800, f0), pp = pitchHz(po, op + a + 4800, f0); printf(" | %+6.0f", 1200 * log2(pn / pp)); }
    printf("\n");
  }
}
