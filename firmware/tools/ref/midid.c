// midid <fifo>: virtual CoreMIDI source "ObsRef" for NS2 reference renders (NS2 listens to every
// MIDI input). Reads commands from the FIFO, one per line:
//   on <note> <vel> | off <note> | cc <num> <val> | pc <prog> | wait <ms> | seq | fxseq | quit
// "seq" and "fxseq" play a sequence of seq.h with timestamps, so the timing does not depend on this process.
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "seq.h"
static MIDIEndpointRef src;
static mach_timebase_info_data_t tb;
static MIDITimeStamp nowPlus(double ms) { return mach_absolute_time() + (uint64_t)(ms * 1e6 * tb.denom / tb.numer); }
static void send3(uint8_t a, uint8_t b, uint8_t c, MIDITimeStamp t) {
  Byte buf[256]; MIDIPacketList* pl = (MIDIPacketList*)buf; MIDIPacket* p = MIDIPacketListInit(pl);
  Byte m[3] = {a, b, c}; p = MIDIPacketListAdd(pl, sizeof(buf), p, t, (a & 0xf0) == 0xc0 ? 2 : 3, m); MIDIReceived(src, pl);
}
int main(int argc, char** argv) {
  mach_timebase_info(&tb);
  MIDIClientRef cl; MIDIClientCreate(CFSTR("ObsRef"), NULL, NULL, &cl);
  MIDISourceCreate(cl, CFSTR("ObsRef"), &src);
  const char* fifo = argv[1]; mkfifo(fifo, 0600);
  fprintf(stderr, "ObsRef ready on %s\n", fifo);
  for (;;) {
    FILE* f = fopen(fifo, "r"); if (!f) return 1; char line[128];
    while (fgets(line, sizeof line, f)) {
      int a = 0, b = 0; char cmd[16] = "";
      sscanf(line, "%15s %d %d", cmd, &a, &b);
      if (!strcmp(cmd, "on")) send3(0x90, a, b, 0);
      else if (!strcmp(cmd, "off")) send3(0x80, a, 0, 0);
      else if (!strcmp(cmd, "cc")) send3(0xb0, a, b, 0);
      else if (!strcmp(cmd, "pc")) send3(0xc0, a, 0, 0);
      else if (!strcmp(cmd, "wait")) usleep(a * 1000);
      else if (!strcmp(cmd, "seq") || !strcmp(cmd, "fxseq")) {
        const int fx = !strcmp(cmd, "fxseq");
        const struct RefEvent* seq = fx ? kFxSeq : kRefSeq;
        const size_t count = fx ? kFxSeqCount : kRefSeqCount;
        MIDITimeStamp t0 = nowPlus(200);
        for (size_t i = 0; i < count; i++) {
          const struct RefEvent* e = &seq[i];
          send3(0x90, e->note, e->vel, t0 + (uint64_t)(e->t * 1e9 * tb.denom / tb.numer));
          send3(0x80, e->note, 0, t0 + (uint64_t)((e->t + e->dur) * 1e9 * tb.denom / tb.numer));
        }
      } else if (!strcmp(cmd, "quit")) return 0;
    }
    fclose(f);
  }
}
