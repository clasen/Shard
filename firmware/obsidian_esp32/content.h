// Content image (firmware/tools/content2bin.py) in the "content" flash partition: samples,
// wavetables and presets, memory-mapped with esp_partition_mmap and read through the cache.
#pragma once

#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "synth.h"

namespace content {

constexpr int kMaxSamples = 256, kMaxTables = 64, kZonePool = 192;

struct Header {
  char magic[4];
  uint32_t version, total;
  uint32_t sampleOff, sampleCount, tableOff, tableCount, pathOff, pathCount, zoneOff, zoneCount, presetOff, presetCount;
};
struct SampleEntry { char name[32]; uint32_t offset, length, loopStart, loopEnd, rate, reserved; };
struct TableEntry { char name[48]; uint32_t offset, frames; float scale; uint32_t reserved; };
struct PathEntry { char path[48]; };
struct ZoneEntry { char file[32]; uint8_t lo, hi, root, loop; float tune, vol; };
struct PresetEntry { char name[40]; uint8_t category, reserved[3]; uint32_t offset, count; };
struct Record { uint16_t path; uint8_t kind, reserved; union { float f; uint32_t u; }; };
// kRecZones packs the first zone in the low 16 bits and the zone count in the high 16.
enum RecordKind : uint8_t { kRecValue, kRecTable, kRecZones, kRecClearMod };

// What a path of the image's path table resolves to.
struct Target { int16_t offset; syn::FieldKind kind; int8_t osc; };  // offset -1: table/zones of `osc`

static const uint8_t* gBase = nullptr;
static const Header* gHeader = nullptr;
static syn::Sample gSamples[kMaxSamples];
static syn::Wavetable gTables[kMaxTables];
static Target* gTargets = nullptr;  // one per path, in PSRAM
static syn::SampleZone gZones[2][kZonePool];  // two pools: voices of the previous preset keep theirs
static int gZoneBank = 0;

template <class T> static const T* at(uint32_t off) { return (const T*)(gBase + off); }

// The offset of every registered Patch field, matched to the image's paths by name.
static bool resolvePaths(const char** why) {
  const uint32_t n = gHeader->pathCount;
  gTargets = (Target*)heap_caps_malloc(sizeof(Target) * (n ? n : 1), MALLOC_CAP_SPIRAM);
  if (!gTargets) { *why = "out of PSRAM"; return false; }
  for (uint32_t i = 0; i < n; i++) gTargets[i] = {-2, syn::kFieldF32, 0};
  static syn::Patch probe;
  const PathEntry* paths = at<PathEntry>(gHeader->pathOff);
  syn::forEachField(probe, [&](const char* name, void* ptr, syn::FieldKind kind) {
    for (uint32_t i = 0; i < n; i++)
      if (!strncmp(paths[i].path, name, sizeof(paths[i].path)))
        gTargets[i] = {(int16_t)((uint8_t*)ptr - (uint8_t*)&probe), kind, 0};
  });
  for (uint32_t i = 0; i < n; i++) {
    int osc;
    char what[8];
    if (gTargets[i].offset == -2 && sscanf(paths[i].path, "osc[%d].%7s", &osc, what) == 2 &&
        (!strcmp(what, "table") || !strcmp(what, "zones")))
      gTargets[i] = {-1, syn::kFieldF32, (int8_t)osc};
    if (gTargets[i].offset == -2 && !strcmp(paths[i].path, "clearMod")) gTargets[i].offset = -1;
    if (gTargets[i].offset == -2) { *why = paths[i].path; return false; }  // a path this firmware lacks
  }
  return true;
}

// Maps the image; returns the number of presets, or a negative value with `why` set.
static int begin(const char** why) {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "content");
  if (!part) { *why = "no 'content' partition (see firmware/tools/partitions_content.csv)"; return -1; }
  Header h;
  if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK || memcmp(h.magic, "OBSC", 4) || h.version != 1) {
    *why = "the 'content' partition holds no image (flash one made by content2bin.py)";
    return -1;
  }
  if (h.total > part->size || h.sampleCount > (uint32_t)kMaxSamples || h.tableCount > (uint32_t)kMaxTables) {
    *why = "image too large for this firmware";
    return -1;
  }
  const void* ptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(part, 0, h.total, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) {
    *why = "esp_partition_mmap failed";
    return -1;
  }
  gBase = (const uint8_t*)ptr;
  gHeader = (const Header*)gBase;
  const SampleEntry* s = at<SampleEntry>(h.sampleOff);
  for (uint32_t i = 0; i < h.sampleCount; i++)
    gSamples[i] = {at<int16_t>(s[i].offset), s[i].length, s[i].loopStart, s[i].loopEnd, (float)s[i].rate / syn::kSampleRate};
  const TableEntry* t = at<TableEntry>(h.tableOff);
  for (uint32_t i = 0; i < h.tableCount; i++)
    gTables[i] = {(int)t[i].frames, (const int16_t(*)[syn::kMips][syn::kTableLen + 1])(gBase + t[i].offset), t[i].scale};
  if (!resolvePaths(why)) { gHeader = nullptr; return -1; }
  return (int)h.presetCount;
}

static int presetCount() { return gHeader ? (int)gHeader->presetCount : 0; }
static const PresetEntry& preset(int i) { return at<PresetEntry>(gHeader->presetOff)[i]; }
static int sampleCount() { return gHeader ? (int)gHeader->sampleCount : 0; }
static int tableCount() { return gHeader ? (int)gHeader->tableCount : 0; }

static const syn::Sample* findSample(const char* name) {
  if (!gHeader) return nullptr;
  const SampleEntry* s = at<SampleEntry>(gHeader->sampleOff);
  for (uint32_t i = 0; i < gHeader->sampleCount; i++)
    if (!strncmp(s[i].name, name, sizeof(s[i].name))) return &gSamples[i];
  return nullptr;
}

// Binds every zone of the patch's Sample oscillators to its sample; returns the zones left unbound.
static int bind(syn::Patch& p) {
  int missing = 0;
  for (auto& o : p.osc) {
    if (o.type != syn::kOscSample) continue;
    for (int z = 0; z < o.zoneCount; z++) {
      if (!o.zones[z].sample) o.zones[z].sample = findSample(o.zones[z].file);
      missing += o.zones[z].sample == nullptr;
    }
  }
  return missing;
}

// Builds content preset i into p: the default Patch, then the preset's records in order.
static void loadPreset(int i, syn::Patch& p) {
  new (&p) syn::Patch();
  const PresetEntry& e = preset(i);
  const Record* r = at<Record>(e.offset);
  gZoneBank ^= 1;
  syn::SampleZone* pool = gZones[gZoneBank];
  int used = 0;
  for (uint32_t k = 0; k < e.count; k++, r++) {
    const Target& t = gTargets[r->path];
    switch (r->kind) {
      case kRecClearMod: p.clearMod(); break;
      case kRecTable: p.osc[t.osc].table = r->u < gHeader->tableCount ? &gTables[r->u] : nullptr; break;
      case kRecZones: {
        const uint32_t first = r->u & 0xffff, count = r->u >> 16;
        const ZoneEntry* z = at<ZoneEntry>(gHeader->zoneOff) + first;
        syn::OscPatch& o = p.osc[t.osc];
        o.zones = pool + used;
        o.zoneCount = 0;
        for (uint32_t j = 0; j < count && first + j < gHeader->zoneCount && used < kZonePool; j++, o.zoneCount++)
          pool[used++] = {z[j].file, z[j].lo, z[j].hi, z[j].root, z[j].tune, z[j].vol, z[j].loop != 0, nullptr};
        break;
      }
      default: {
        uint8_t* f = (uint8_t*)&p + t.offset;
        switch (t.kind) {
          case syn::kFieldF32: *(float*)f = r->f; break;
          case syn::kFieldI32: *(int32_t*)f = (int32_t)r->f; break;
          case syn::kFieldI8: *(int8_t*)f = (int8_t)r->f; break;
          case syn::kFieldU8: *(uint8_t*)f = (uint8_t)r->f; break;
          case syn::kFieldBool: *(bool*)f = r->f != 0.0f; break;
        }
      }
    }
  }
  bind(p);
}

}  // namespace content
