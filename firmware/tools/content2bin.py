#!/usr/bin/env python3
"""Builds the content image of the ESP32 port: the NS2 Obsidian presets as data, plus the
wavetables and samples they use. The firmware maps it from the "content" flash partition
(firmware/obsidian_esp32/content.h) and lists its presets after the built-in ones.

  python3 -I content2bin.py <out.bin> [--samples <preset.obs> ...] <preset.obs | folder> ...

Folders are searched for .obs presets. A preset is left out, and listed, when one of its
oscillators has a type the engine lacks (obs2preset.py would disable it), or when it plays
samples and was not named after --samples (samples are large: name only the presets whose
samples should go into the image).

Each preset is the C++ obs2preset.py writes, stored as records: the field path ("osc[0].level")
in a shared path table, and the value. Layout, little endian:

  header   "OBSC", u32 version (1), u32 total, then (u32 offset, u32 count) for samples,
           wavetables, paths, zones and presets
  sample   char name[32], u32 offset, length, loop start, loop end (exclusive; 0 = none), rate, 0
  table    char name[48], u32 offset, u32 frames, f32 scale, 0; data int16[frames][11][1025]
  path     char path[48]
  zone     char file[32], u8 lo, hi, root, loop, f32 tune, vol
  preset   char name[40], u8 category, 3 x u8 0, u32 offset, u32 record count
  record   u16 path, u8 kind (0 value, 1 wavetable index, 2 zones: first | count << 16,
           3 clearMod), u8 0, f32 value or u32
  data     every block starts on a 4-byte boundary

Flash it at the offset of the "content" partition (firmware/tools/partitions_content.csv):

  esptool --chip esp32s3 --port <port> write-flash 0x210000 <out.bin>

The output is derived from NanoStudio 2 content and the presets' samples: keep it local.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import obs2preset  # noqa: E402
import tbl2h  # noqa: E402

FIRMWARE = os.path.join(HERE, "..", "obsidian_esp32")
PARTITION_SIZE = 0xDE0000  # "content" in partitions_content.csv
HEADER = struct.Struct("<4s12I")
SAMPLE = struct.Struct("<32s6I")
TABLE = struct.Struct("<48s2IfI")
PATH = struct.Struct("<48s")
ZONE = struct.Struct("<32s4B2f")
PRESET = struct.Struct("<40sB3x2I")
RECORD_VALUE, RECORD_TABLE, RECORD_ZONES, RECORD_CLEAR = 0, 1, 2, 3

# Pad categories, in the order of the Korg pads (obsidian_esp32.ino). NS2's Tags decide first: in
# the factory presets bit 19 marks drums (24 of 27 named so), 2 bass (39/41), 0 lead (9/9), 1 pad
# and 10 keys; the other bits describe timbre. Then keywords of the preset or folder name.
TAG_CATEGORIES = [(19, "Drums"), (2, "Bass"), (0, "Lead"), (1, "Pad"), (10, "Keys")]
CATEGORIES = [
    ("Drums", r"kick|snare|hat|clap|perc|drum|tom\b|cymb|rim|cowbell|shaker|conga|808|909"),
    ("Bass", r"bass|sub\b|303|202|101"),
    ("Keys", r"key|piano|organ|rhodes|\bep\b|e\.piano|wurl|clav|bell|mallet|marimba|vibe|chime"),
    ("Pluck", r"pluck|pizz|arp|stab"),
    ("Lead", r"lead|solo|sync"),
    ("Pad", r"pad|atmo|drone|ambient|choir|sweep|evolv|space|dream"),
    ("Brass", r"brass|horn|trump|string|orch|ensemble|violin|cello"),
]
PAD_ORDER = ["Bass", "Lead", "Pad", "Pluck", "Brass", "Other", "Drums", "Keys"]


def category(name, folder, tags):
    text = f"{name} {folder}".lower()
    for cat, pattern in CATEGORIES:  # Tags have no pluck or brass category: these names come first
        if cat in ("Pluck", "Brass") and re.search(pattern, text):
            return PAD_ORDER.index(cat)
    for bit, cat in TAG_CATEGORIES:
        if tags >> bit & 1:
            return PAD_ORDER.index(cat)
    for cat, pattern in CATEGORIES:
        if re.search(pattern, text):
            return PAD_ORDER.index(cat)
    return PAD_ORDER.index("Other")


def engine_values():
    """Names usable in the generated C++: constexpr ints, enumerators and the helper functions."""
    src = re.sub(r"//[^\n]*", "", open(os.path.join(FIRMWARE, "synth.h")).read())
    env = {}
    for name, expr in re.findall(r"constexpr int (\w+) = ([^;]+);", src):
        try:
            env[name] = eval(expr, {}, env)
        except NameError:
            pass
    for body in re.findall(r"enum \w+(?:\s*:\s*\w+)?\s*\{([^}]*)\}", src):
        value = -1
        for item in body.split(","):
            item = item.strip()
            if not item:
                continue
            name, _, expr = item.partition("=")
            value = eval(expr, {}, env) if expr.strip() else value + 1
            env[name.strip()] = value
    for name, expr in re.findall(r"constexpr int (\w+) = ([^;]+);", src):
        env.setdefault(name, eval(expr, {}, env))
    env.update(srcEnv=lambda i: env["kSrcEnv1"] + i, srcLfo=lambda i: env["kSrcLfo1"] + i,
               srcMacro=lambda i: env["kSrcMacro1"] + i, dstFmConn=lambda i: env["kDstFmConn"] + i,
               dstFmRatio=lambda i: env["kDstFmRatio"] + i, true=1, false=0)
    return env


def number(expr, env):
    expr = expr.strip()
    if re.fullmatch(r"[-+]?[\d.]+(e[-+]?\d+)?f?", expr):
        return float(expr.rstrip("f"))
    return float(eval(expr, {}, env))


def statements(code):
    """Yield the statements of the generated C++ (one or more per line, comments dropped)."""
    zone_block = None
    for line in code:
        line = line.split("//")[0].strip() if '"' not in line else line.strip()
        if zone_block is not None:
            if line.startswith("};"):
                yield ("zones", zone_block)
                zone_block = None
            else:
                m = re.match(r'\{"(.*)", (\d+), (\d+), (\d+), ([^,]+), ([^,]+), (true|false), nullptr\},', line)
                zone_block[1].append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)),
                                      float(m.group(5).rstrip("f")), float(m.group(6).rstrip("f")), m.group(7) == "true"))
            continue
        m = re.match(r"static SampleZone (\w+)\[\] = \{", line)
        if m:
            zone_block = (m.group(1), [])
            continue
        depth, cur = 0, ""
        for ch in line:
            depth += ch == "{"
            depth -= ch == "}"
            if ch == ";" and depth == 0:
                if cur.strip():
                    yield ("stmt", cur.strip())
                cur = ""
            else:
                cur += ch


class Image:
    def __init__(self):
        self.env = engine_values()
        self.samples = {}   # name -> path
        self.tables = {}    # NS2 name -> index
        self.paths = {}     # path -> index
        self.zones = []     # (file, lo, hi, root, loop, tune, vol)
        self.presets = []   # (name, category, records)

    def path(self, p):
        return self.paths.setdefault(p, len(self.paths))

    def add(self, path, sample_folders):
        """Converts one preset; returns why it was left out, or None."""
        root = obs2preset.load(path)
        name = os.path.splitext(os.path.basename(path))[0]
        conv = obs2preset.Converter(root, name)
        conv.run()
        if any("oscillator disabled" in line for line in conv.report):
            return "an oscillator type the engine lacks"
        if any("app resource" in line for line in conv.report):
            return "plays NanoStudio's own samples (/AppRes), not in the image"
        folder = path if os.path.isdir(path) else os.path.dirname(path)
        if conv.samples and os.path.realpath(folder) not in sample_folders:
            return "plays samples (name it after --samples to include them)"
        for s in conv.samples:
            wav = os.path.join(folder, s)
            if not os.path.exists(wav):
                return f"sample {s} not found"
            if s in self.samples and os.path.realpath(self.samples[s]) != os.path.realpath(wav):
                return f"sample name {s} clashes with another preset's"
            self.samples.setdefault(s, wav)
        symbols = {f"&k{sym}": wt for wt, sym in conv.tables.items()}
        zone_lists, records = {}, []
        for kind, body in statements(conv.code):
            if kind == "zones":
                zone_lists[body[0]] = (len(self.zones), len(body[1]))
                self.zones += [(f, lo, hi, rt, lp, tn, vl) for f, lo, hi, rt, tn, vl, lp in body[1]]
                continue
            if body == "p.clearMod()":
                records.append((self.path("clearMod"), RECORD_CLEAR, 0))
                continue
            lhs, _, rhs = body.partition("=")
            field, rhs = lhs.strip()[2:], rhs.strip()  # drop "p."
            if rhs in symbols:
                wt = symbols[rhs]
                records.append((self.path(field), RECORD_TABLE, self.tables.setdefault(wt, len(self.tables))))
            elif rhs in zone_lists:
                first, count = zone_lists[rhs]
                records.append((self.path(field), RECORD_ZONES, first | count << 16))
            elif field.endswith(".zoneCount"):
                continue  # the zones record carries the count
            elif rhs.startswith("{"):
                members = ("src", "scale", "dest", "index", "depth")
                for member, value in zip(members, rhs.strip("{} ").split(",")):
                    records.append((self.path(f"{field}.{member}"), RECORD_VALUE, number(value, self.env)))
            else:
                records.append((self.path(field), RECORD_VALUE, number(rhs, self.env)))
        tags = int(root.get("Tags", "0"))
        self.presets.append((name, category(name, os.path.basename(os.path.dirname(path)), tags), records))
        return None

    def write(self, out_path):
        blob = bytearray(HEADER.size)
        sections = []

        def align():
            blob.extend(b"\0" * (-len(blob) % 4))

        # Samples
        entries, data = [], []
        for name, wav in self.samples.items():
            if len(name.encode()) >= 32:
                sys.exit(f"{name}: sample name longer than 31 bytes")
            mono, rate, loop = read_wav(wav)
            entries.append((name, mono, rate, loop))
        table_at = len(blob)
        blob += b"\0" * (SAMPLE.size * len(entries))
        rows = []
        for name, mono, rate, loop in entries:
            align()
            start, end = loop if loop else (0, 0)
            rows.append(SAMPLE.pack(name.encode(), len(blob), len(mono) // 2, start, end, rate, 0))
            blob += mono
        blob[table_at:table_at + len(b"".join(rows))] = b"".join(rows)
        sections.append((table_at, len(entries)))
        # Wavetables
        align()
        table_at = len(blob)
        names = sorted(self.tables, key=self.tables.get)
        blob += b"\0" * (TABLE.size * len(names))
        rows = []
        for wt in names:
            tbl = os.path.join(obs2preset.WAVETABLES, wt + ".tbl")
            frames, scale, frame_data = tbl2h.convert(tbl)
            align()
            rows.append(TABLE.pack(wt.encode(), len(blob), frames, scale, 0))
            for frame in frame_data:
                for mip in frame:
                    blob += struct.pack(f"<{len(mip)}h", *mip)
        blob[table_at:table_at + len(b"".join(rows))] = b"".join(rows)
        sections.append((table_at, len(names)))
        # Paths, zones, presets
        align()
        sections.append((len(blob), len(self.paths)))
        for p in sorted(self.paths, key=self.paths.get):
            if len(p) >= 48:
                sys.exit(f"{p}: path longer than 47 bytes")
            blob += PATH.pack(p.encode())
        align()
        sections.append((len(blob), len(self.zones)))
        for f, lo, hi, rt, lp, tn, vl in self.zones:
            blob += ZONE.pack(f.encode(), lo, hi, rt, 1 if lp else 0, tn, vl)
        align()
        table_at = len(blob)
        blob += b"\0" * (PRESET.size * len(self.presets))
        rows = []
        for name, cat, records in self.presets:
            align()
            rows.append(PRESET.pack(name.encode()[:39], cat, len(blob), len(records)))
            for path, kind, value in records:
                packed = struct.pack("<f", value) if kind == RECORD_VALUE else struct.pack("<I", value)
                blob += struct.pack("<HBx", path, kind) + packed
        blob[table_at:table_at + len(b"".join(rows))] = b"".join(rows)
        sections.append((table_at, len(self.presets)))
        align()
        flat = [v for sec in sections for v in sec]
        blob[0:HEADER.size] = HEADER.pack(b"OBSC", 1, len(blob), *flat)
        if len(blob) > PARTITION_SIZE:
            sys.exit(f"image is {len(blob)} bytes, the content partition holds {PARTITION_SIZE}")
        with open(out_path, "wb") as f:
            f.write(blob)
        return len(blob)


def read_wav(path):
    """Mono int16 samples (bytes), sample rate and (loop start, loop end exclusive) or None."""
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    fmt = data = loop = None
    p = 12
    while p + 8 <= len(d):
        cid, size = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
        body = d[p + 8:p + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif cid == b"data":
            data = body
        elif cid == b"smpl":
            count = struct.unpack_from("<I", body, 28)[0]
            for i in range(count):
                _, kind, start, end, _, _ = struct.unpack_from("<6I", body, 36 + 24 * i)
                if kind == 0 and end >= start:  # first forward loop; smpl end is inclusive
                    loop = (start, end + 1)
                    break
        p += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.exit(f"{path}: no fmt or data chunk")
    tag, channels, rate, _, _, bits = fmt
    if tag not in (1, 0xFFFE) or bits not in (16, 24):
        sys.exit(f"{path}: only 16/24-bit PCM is supported (format {tag}, {bits} bits)")
    width = bits // 8
    frames = len(data) // (width * channels)
    if bits == 16 and channels == 1:
        mono = data[:frames * 2]
    else:
        out = bytearray()
        for f in range(frames):
            acc = 0
            for c in range(channels):
                o = (f * channels + c) * width
                acc += int.from_bytes(data[o:o + width], "little", signed=True) >> (bits - 16)
            out += struct.pack("<h", acc // channels)
        mono = bytes(out)
    if loop and loop[1] > frames:
        sys.exit(f"{path}: loop end {loop[1]} beyond {frames} frames")
    return mono, rate, loop


def presets_in(path):
    if path.endswith(".obs"):
        return [path]
    found = []
    for dirpath, dirs, files in os.walk(path):
        for d in dirs:
            if d.endswith(".obs"):
                found.append(os.path.join(dirpath, d))
        found += [os.path.join(dirpath, f) for f in files if f.endswith(".obs") and f != "Package.obs"]
        dirs[:] = [d for d in dirs if not d.endswith(".obs")]
    return sorted(found)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    out_path, sample_presets, sources = args[0], [], []
    i = 1
    while i < len(args):
        if args[i] == "--samples":
            sample_presets.append(args[i + 1].rstrip("/"))
            i += 2
        else:
            sources.append(args[i].rstrip("/"))
            i += 1
    sample_folders = {os.path.realpath(p if os.path.isdir(p) else os.path.dirname(p)) for p in sample_presets}
    image, skipped = Image(), []
    for src in sources + sample_presets:
        for preset in presets_in(src):
            if any(name == os.path.splitext(os.path.basename(preset))[0] for name, _, _ in image.presets):
                continue
            why = image.add(preset, sample_folders)
            if why:
                skipped.append((preset, why))
    size = image.write(out_path)
    counts = [0] * len(PAD_ORDER)
    for _, cat, _ in image.presets:
        counts[cat] += 1
    print(f"{out_path}: {len(image.presets)} presets, {len(image.tables)} wavetables, {len(image.samples)} samples, "
          f"{size} bytes ({100 * size / PARTITION_SIZE:.0f}% of the partition)")
    print("  per pad: " + ", ".join(f"{c} {n}" for c, n in zip(PAD_ORDER, counts)))
    print(f"Left out ({len(skipped)}):", file=sys.stderr)
    for preset, why in skipped:
        print(f"  {os.path.basename(preset)}: {why}", file=sys.stderr)


if __name__ == "__main__":
    main()
