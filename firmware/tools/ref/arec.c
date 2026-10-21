// arec <device name> <seconds> <out.raw>: records the named input device as 48 kHz mono float32
// with an AudioQueue (Core Audio converts the format), so the timeline is exact.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static FILE* out; static long want, got;
static void cb(void* u, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp* t, UInt32 n, const AudioStreamPacketDescription* d) {
  long k = b->mAudioDataByteSize / 4; if (got + k > want) k = want - got;
  if (k > 0) { fwrite(b->mAudioData, 4, k, out); got += k; }
  if (got < want) AudioQueueEnqueueBuffer(q, b, 0, NULL);
}
static CFStringRef deviceUID(const char* name) {
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
  AudioDeviceID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
  for (UInt32 i = 0; i < sz / sizeof(AudioDeviceID); i++) {
    CFStringRef n = NULL; UInt32 s = sizeof n; AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &n); char buf[256] = ""; if (n) CFStringGetCString(n, buf, sizeof buf, kCFStringEncodingUTF8);
    if (!strcmp(buf, name)) { CFStringRef uid = NULL; s = sizeof uid; AudioObjectPropertyAddress ua = {kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
      AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &s, &uid); return uid; }
  }
  return NULL;
}
int main(int argc, char** argv) {
  AudioStreamBasicDescription f = {48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 4, 1, 4, 1, 32, 0};
  AudioQueueRef q; if (AudioQueueNewInput(&f, cb, NULL, NULL, NULL, 0, &q)) { fprintf(stderr, "AudioQueueNewInput failed\n"); return 1; }
  CFStringRef uid = deviceUID(argv[1]); if (!uid) { fprintf(stderr, "no device %s\n", argv[1]); return 1; }
  if (AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &uid, sizeof uid)) { fprintf(stderr, "set device failed\n"); return 1; }
  want = (long)(atof(argv[2]) * 48000); out = fopen(argv[3], "wb");
  for (int i = 0; i < 4; i++) { AudioQueueBufferRef b; AudioQueueAllocateBuffer(q, 4800 * 4, &b); AudioQueueEnqueueBuffer(q, b, 0, NULL); }
  AudioQueueStart(q, NULL);
  while (got < want) usleep(10000);
  AudioQueueStop(q, true); fclose(out); return 0;
}
