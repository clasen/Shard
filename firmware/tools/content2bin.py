#!/usr/bin/env python3
"""Builds the content image of the ESP32 port: the NS2 Obsidian presets and Slate kits as data,
plus the wavetables and samples they use. The firmware maps it from the "content" flash partition
(firmware/obsidian_esp32/content.h) and lists its presets after the built-in ones.

  python3 -I content2bin.py <out.bin> [--samples <preset.obs> ...] [--thin <preset.obs> ...]
                            <preset.obs | kit.slt | folder> ...

Folders are searched for .obs presets and .slt kits. A preset is left out, and listed, when one
of its oscillators has a type the engine lacks (obs2preset.py would disable it), or when it plays
samples and was not named after --samples (samples are large: name only the presets whose
samples should go into the image). --thin also takes a sample preset and keeps every other key
zone of each oscillator, stretching the kept zones over the dropped ones: about half the samples,
each pitched further.

A Slate kit becomes a preset of the Drums pad named "Kit <name>" whose one record points at the
kit. Its pads play their .wav samples; .mwav samples (NS2's multisample format) are left out and
listed with the parameters the firmware does not play.

Each preset is the C++ obs2preset.py writes, stored as records: the field path ("osc[0].level")
in a shared path table, and the value. Layout, little endian:

  header   "OBSC", u32 version (2), u32 total, then (u32 offset, u32 count) for samples,
           wavetables, paths, zones, presets and kits
  sample   char name[32], u32 offset, length, loop start, loop end (exclusive; 0 = none), rate, 0
  table    char name[48], u32 offset, u32 frames, f32 scale, 0; data int16[frames][11][1025]
  path     char path[48]
  zone     char file[32], u8 lo, hi, root, loop, f32 tune, vol
  preset   char name[40], u8 category, 3 x u8 0, u32 offset, u32 record count
  record   u16 path, u8 kind (0 value, 1 wavetable index, 2 zones: first | count << 16,
           3 clearMod, 4 kit index), u8 0, f32 value or u32
  kit      char name[40], u8 group polyphony[32] (0 = no limit), 32 pads
  pad      u8 note, group, filter type (0 LP 6, 1 LP 12, 2 HP 6, 3 HP 12, 4 BP), filter on,
           f32 level, pan, attack, hold, decay, break level, decay 2 (decays < 0: none),
           velocity to level, pitch (semitones), cutoff, Q, velocity to cutoff,
           u8 bus filter type, bus filter on, 2 x u8 0, f32 bus cutoff, bus Q, reverb send,
           3 samples
  pad sample  i32 sample index (-1 = none), f32 level, pan, pitch (semitones), start offset,
           u8 reverse, 3 x u8 0
  data     every block starts on a 4-byte boundary

Flash it at the offset of the "content" partition (firmware/tools/partitions_content.csv):

  esptool --chip esp32s3 --port <port> write-flash 0x210000 <out.bin>

The output is derived from NanoStudio 2 content and the presets' samples: keep it local.
"""
import hashlib
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import obs2preset  # noqa: E402
import tbl2h  # noqa: E402

FIRMWARE = os.path.join(HERE, "..", "obsidian_esp32")
PARTITION_SIZE = 0xDE0000  # "content" in partitions_content.csv
HEADER = struct.Struct("<4s14I")
SAMPLE = struct.Struct("<32s6I")
TABLE = struct.Struct("<48s2IfI")
PATH = struct.Struct("<48s")
ZONE = struct.Struct("<32s4B2f")
PRESET = struct.Struct("<40sB3x2I")
KIT = struct.Struct("<40s32B")
KIT_PAD = struct.Struct("<4B12f2B2x3f")
KIT_SAMPLE = struct.Struct("<i4fB3x")
RECORD_VALUE, RECORD_TABLE, RECORD_ZONES, RECORD_CLEAR, RECORD_KIT = 0, 1, 2, 3, 4
NS2 = "/Applications/NanoStudio 2.app/Wrapper/NanoStudio 2.app"
KIT_FILTERS = {"LP 6": 0, "LP 12": 1, "HP 6": 2, "HP 12": 3, "BP": 4}

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
        self.kits = []      # (name, group polyphony, pads)

    def path(self, p):
        return self.paths.setdefault(p, len(self.paths))

    def add(self, path, sample_folders, thin=False):
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
        stmts = list(statements(conv.code))
        if thin:
            stmts = [(kind, (body[0], thin_zones(body[1])) if kind == "zones" else body) for kind, body in stmts]
        used = {z[0] for kind, body in stmts if kind == "zones" for z in body[1]}
        for s in conv.samples:
            if s not in used:
                continue
            wav = os.path.join(folder, s)
            if not os.path.exists(wav):
                return f"sample {s} not found"
            if s in self.samples and os.path.realpath(self.samples[s]) != os.path.realpath(wav):
                return f"sample name {s} clashes with another preset's"
            self.samples.setdefault(s, wav)
        symbols = {f"&k{sym}": wt for wt, sym in conv.tables.items()}
        zone_lists, records = {}, []
        for kind, body in stmts:
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

    def add_kit(self, path):
        """Converts one Slate kit into a kit and its Drums preset; returns what it does not play."""
        root = ET.parse(os.path.join(path, "Package.slt")).getroot()
        name = "Kit " + os.path.splitext(os.path.basename(path))[0]
        fx = root.find("FX")
        midi, groups = root.find("MIDI"), root.find("ExGrp")
        poly = [int(groups.get(f"P{g + 1:02d}", "0")) for g in range(32)]
        gaps, pads = set(), []
        for pad in root.find("Pads"):
            amp, flt, pitch, osc = pad.find("Amp"), pad.find("Filt"), pad.find("Pitch"), pad.find("Osc")
            samples = []
            for o in osc:
                ref = o.find("Samp/Smpl")
                f = ref.get("File") if ref is not None else ""
                if not f or f.endswith(".mwav"):
                    if f:
                        gaps.add(".mwav samples")
                    samples.append(None)
                    continue
                wav = f.replace("/AppRes", NS2) if f.startswith("/") else os.path.join(path, f)
                key = os.path.splitext(os.path.basename(wav))[0][:24] + "~" + hashlib.md5(
                    os.path.realpath(wav).encode()).hexdigest()[:6]
                self.samples.setdefault(key, wav)
                samp = o.find("Samp")
                samples.append((key, num(o.get("Lev")), num(o.get("Pan")), num(o.get("Trn")) + num(o.get("Dtn")),
                                num(samp.get("SOff")), samp.get("SDir") == "Reverse"))
            if osc.get("Type") != "Layer":
                gaps.add(f"velocity split {osc.get('Type')} (layered)")
            if pad.get("TM") != "One-Shot":
                gaps.add(f"trigger mode {pad.get('TM')} (one-shot)")
            bus = fx.find(f"Bus{pad.get('OBus')}") if pad.get("OBus") not in (None, "None") else None
            bfilt = bus.find("Filt") if bus is not None else None
            btype = KIT_FILTERS.get(bfilt.get("Type")) if bfilt is not None else None
            bon = bfilt is not None and bfilt.get("En") == "1" and btype is not None
            send = num(bus.get("S1")) if bus is not None and bus.get("SEn") == "1" else 0.0
            if bus is not None and any(e is not None and e.get("En") == "1" for e in (bus.find("EQ"), bus.find("Comp"))):
                gaps.add("bus EQ/compressor")
            if bus is not None and bus.get("SEn") == "1" and num(bus.get("S2")) > 0:
                gaps.add("delay sends")
            if pad.find("WS") is not None and pad.find("WS").get("En") == "On":
                gaps.add("waveshaper")
            if num(pitch.get("AL")) != 0:
                gaps.add("pitch envelope")
            if num(flt.get("AL")) != 0:
                gaps.add("filter envelope")
            ftype = KIT_FILTERS.get(flt.get("Type"))
            if flt.get("En") == "On" and ftype is None:
                gaps.add(f"filter {flt.get('Type')}")
            inf = lambda v: -1.0 if v == "INF" else num(v)
            pads.append((int(midi.get(pad.tag, "0")), int(pad.get("Grp", "0")), ftype or 0,
                         1 if flt.get("En") == "On" and ftype is not None else 0,
                         (num(pad.get("Lev")), num(pad.get("Pan")), num(amp.get("A")), num(amp.get("H")),
                          inf(amp.get("D")), num(amp.get("S")), inf(amp.get("D2")), num(amp.get("VL")),
                          num(pitch.get("Trn")) + num(pitch.get("Dtn")), num(flt.get("CutO")), num(flt.get("Q")),
                          num(flt.get("Vel"))),
                         (btype or 0, 1 if bon else 0, num(bfilt.get("CutO")) if bon else 0.0,
                          num(bfilt.get("Q")) if bon else 0.0, send),
                         samples))
        main = fx.find("Main")
        if any(e is not None and e.get("En") == "1" for e in (main.find("EQ"), main.find("Comp"))):
            gaps.add("main EQ/compressor")
        # The bus sends feed the kit's reverb, heard at the main bus's return level.
        sends = any(b[4] > 0 for _, _, _, _, _, b, _ in pads)
        records = [(self.path("kit"), RECORD_KIT, len(self.kits)),
                   (self.path("reverbMix"), RECORD_VALUE,
                    num(main.get("R1")) if sends and main.get("REn") == "1" else 0.0)]
        for kind, body in statements(obs2preset.reverb_fields(fx.find("Rev")).splitlines()):
            lhs, _, rhs = body.partition("=")
            records.append((self.path(lhs.strip()[2:]), RECORD_VALUE, number(rhs.strip(), self.env)))
        self.presets.append((name, PAD_ORDER.index("Drums"), records))
        self.kits.append((name, poly, pads))
        return sorted(gaps)

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
        # Kits
        align()
        sections.append((len(blob), len(self.kits)))
        index = {name: i for i, name in enumerate(self.samples)}
        for name, poly, pads in self.kits:
            blob += KIT.pack(name.encode()[:39], *poly)
            for note, group, ftype, fon, values, bus, samples in pads:
                blob += KIT_PAD.pack(note, group, ftype, fon, *values, *bus)
                for s in samples:
                    blob += KIT_SAMPLE.pack(-1, 0, 0.5, 0, 0, 0) if s is None else KIT_SAMPLE.pack(index[s[0]], *s[1:5], s[5])
        align()
        flat = [v for sec in sections for v in sec]
        blob[0:HEADER.size] = HEADER.pack(b"OBSC", 2, len(blob), *flat)
        if len(blob) > PARTITION_SIZE:
            sys.exit(f"image is {len(blob)} bytes, the content partition holds {PARTITION_SIZE}")
        with open(out_path, "wb") as f:
            f.write(blob)
        return len(blob)


def num(v):
    return float(v) if v not in (None, "") else 0.0


def thin_zones(zones):
    """Every other zone of a key-zone list (f, lo, hi, root, tune, vol, loop); each dropped zone's
    range goes half to the zone below it and half to the one above."""
    z = sorted(zones, key=lambda t: t[1])
    if len(z) < 3:
        return z
    keep = [list(t) for t in z[::2]]
    for j in range(1, len(z), 2):
        if j == len(z) - 1:
            keep[-1][2] = z[j][2]
            continue
        mid = (z[j][1] + z[j][2]) // 2
        keep[(j - 1) // 2][2], keep[(j + 1) // 2][1] = mid, mid + 1
    return [tuple(t) for t in keep]


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
    if path.endswith((".obs", ".slt")):
        return [path]
    found = []
    for dirpath, dirs, files in os.walk(path):
        for d in dirs:
            if d.endswith((".obs", ".slt")):
                found.append(os.path.join(dirpath, d))
        found += [os.path.join(dirpath, f) for f in files if f.endswith(".obs") and f != "Package.obs"]
        dirs[:] = [d for d in dirs if not d.endswith((".obs", ".slt"))]
    return sorted(found)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    out_path, sample_presets, thin, sources = args[0], [], set(), []
    i = 1
    while i < len(args):
        if args[i] in ("--samples", "--thin"):
            sample_presets.append(args[i + 1].rstrip("/"))
            if args[i] == "--thin":
                thin.add(os.path.realpath(args[i + 1].rstrip("/")))
            i += 2
        else:
            sources.append(args[i].rstrip("/"))
            i += 1
    sample_folders = {os.path.realpath(p if os.path.isdir(p) else os.path.dirname(p)) for p in sample_presets}
    image, skipped, partial = Image(), [], []
    found = [preset for src in sources + sample_presets for preset in presets_in(src)]
    # Kits first: the Drums pad starts at the first one.
    for preset in sorted(found, key=lambda p: not p.endswith(".slt")):
        if preset.endswith(".slt"):
            gaps = image.add_kit(preset)
            if gaps:
                partial.append((preset, ", ".join(gaps)))
            continue
        if any(name == os.path.splitext(os.path.basename(preset))[0] for name, _, _ in image.presets):
            continue
        why = image.add(preset, sample_folders, os.path.realpath(preset) in thin)
        if why:
            skipped.append((preset, why))
    size = image.write(out_path)
    counts = [0] * len(PAD_ORDER)
    for _, cat, _ in image.presets:
        counts[cat] += 1
    print(f"{out_path}: {len(image.presets)} presets ({len(image.kits)} kits), {len(image.tables)} wavetables, "
          f"{len(image.samples)} samples, "
          f"{size} bytes ({100 * size / PARTITION_SIZE:.0f}% of the partition)")
    print("  per pad: " + ", ".join(f"{c} {n}" for c, n in zip(PAD_ORDER, counts)))
    print(f"Left out ({len(skipped)}):", file=sys.stderr)
    for preset, why in skipped:
        print(f"  {os.path.basename(preset)}: {why}", file=sys.stderr)
    if partial:
        print(f"Kits played without some of their parameters ({len(partial)}):", file=sys.stderr)
    for preset, gaps in partial:
        print(f"  {os.path.basename(preset)}: {gaps}", file=sys.stderr)


if __name__ == "__main__":
    main()
