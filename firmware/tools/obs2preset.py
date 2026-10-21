#!/usr/bin/env python3
"""Converts an NS2 Obsidian preset (.obs XML, or a .obs folder holding Package.obs) into C++
statements that fill a syn::Patch `p` (firmware/obsidian_esp32/synth.h), meant to be pasted
into a case of makePreset() in obsidian_esp32.ino, where `using namespace syn;` is in scope.

  python3 -I obs2preset.py <preset.obs>

The C++ goes to stdout. The report goes to stderr: the wavetables to convert with tbl2h.py, the
sample image to build with samples2bin.py and every parameter, mod-matrix connection and effect
that the engine cannot represent.
Formulas and attribute meanings: docs/obsidian/02-preset-format.md .. 07, 10-porting-notes.md.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, "..", "obsidian_esp32")
WAVETABLES = "/Applications/NanoStudio 2.app/Wrapper/NanoStudio 2.app/WaveTables"

ANALOG_WAVES = {"Sine": "kSine", "Tri": "kTri", "Saw": "kSaw", "Square": "kSquare", "Pulse": "kPulse"}
NOISE_TYPES = {"White": "kNoiseWhite", "Low Passed": "kNoiseLowPass", "High Passed": "kNoiseHighPass",
               "Band Passed": "kNoiseBandPass", "Notched": "kNoiseNotch"}
FILTER_TYPES = {"LP 24-A": "kFilterLP24A", "LP 12-D": "kFilterLP12D", "LP 12-A": "kFilterLP12A",
                "HP 12-A": "kFilterHP12A", "HP 24-A": "kFilterHP24A", "HP 12-D": "kFilterHP12D",
                "BP 6-A": "kFilterBP6A", "BP 12-A": "kFilterBP12A", "Notch 6-A": "kFilterNotch6A",
                "Notch 12-A": "kFilterNotch12A",
                "EQ Peak": "kFilterEQPeak", "EQ Shelf": "kFilterEQShelf", "WS Tube": "kFilterWSTube",
                "WS Soft": "kFilterWSSoft", "WS Hard": "kFilterWSHard", "WS Enhance": "kFilterWSEnhance",
                "WS Sine": "kFilterWSSine", "WS Tri": "kFilterWSTri", "Formant-M": "kFilterFormantM",
                "Formant-F": "kFilterFormantF"}


CHORUS_TYPES = {"Chorus 1": "kChorus1", "Chorus 2": "kChorus2", "Ensemble 1": "kEnsemble1", "Ensemble 2": "kEnsemble2",
                "Flanger 1": "kFlanger1", "Flanger 2": "kFlanger2"}
# Reverb types (07-effects.md): engine (room = Freeverb, else the Dattorro plate), and the pre-delay
# (s) and its crossfade from the Algoverb preset of the same name.
REVERB_TYPES = {"Small Room 1": ("small", 0, 0), "Small Room 2": ("small", 0.0421, 0.495),
                "Large Room 1": ("large", 0, 0), "Large Room 2": ("large", 0.0421, 0.495),
                "Dense Room 1": ("dense", 0, 0), "Dense Room 2": ("dense", 0.0421, 0.495),
                "Plate 1": ("plate", 0, 0), "Plate 2": ("plate", 0.0928, 0.402), "Hall": ("plate", 0.35, 0.303)}
# Engine feedback at Decy 0.1, 0.5 and 0.9, chosen so that the port's decay rate matches NS2 renders
# (plate 81/27/5.9 dB/s, dense 168/57/17, small >400/61/18, large >400/81/35) [P].
REVERB_DECAY = {"plate": (0.035, 0.55, 0.905), "dense": (0.0, 0.24, 0.708),
                "small": (0.1, 0.76, 0.932), "large": (0.1, 0.69, 0.875)}
# Wet level at Mix 1 at the same Decy points, fitted to the early wet level of NS2 renders [P].
REVERB_GAIN = {"plate": (3.4, 2.26, 2.0), "dense": (0.49, 3.8, 3.0), "small": (19.5, 19.5, 22.6), "large": (13.5, 13.5, 12.7)}
# High damping of the loop at HDmp 0.25 and 0.8 [P].
REVERB_DAMP = {"plate": (0.45, 0.9), "dense": (0.45, 0.9), "small": (0.15, 0.55), "large": (0.15, 0.55)}


def interp3(points, decy):
    """Piecewise-linear through (0.1, 0.5, 0.9) -> points, flat outside."""
    xs = (0.1, 0.5, 0.9)
    if decy <= xs[0]: return points[0]
    if decy >= xs[2]: return points[2]
    i = 0 if decy < xs[1] else 1
    return points[i] + (points[i + 1] - points[i]) * (decy - xs[i]) / (xs[i + 1] - xs[i])


def reverb_fields(rev):
    """The engine values of a Rev element: decay, damping, gain, the HCut/LCut corners and the pre-delay."""
    kind, pre, premix = REVERB_TYPES[rev.get("Type")]
    decy, hdmp = num(rev.get("Decy")), num(rev.get("HDmp"))
    d0, d1 = REVERB_DAMP[kind]
    damp = min(max(d0 + (d1 - d0) * (hdmp - 0.25) / 0.55, 0.0), 0.95)
    hp = 20 + 12000 * num(rev.get("HCut")) ** 3       # HCut is a high-pass, LCut a low-pass (07-effects.md)
    lp = 200 + 21000 * (1 - num(rev.get("LCut"))) ** 3
    return (f"p.reverbDecay = {f(interp3(REVERB_DECAY[kind], decy))}; p.reverbRoom = {'true' if kind in ('small', 'large') else 'false'}; "
            f"p.reverbDamp = {f(damp)}; p.reverbGain = {f(interp3(REVERB_GAIN[kind], decy))};\n"
            f"p.reverbHpHz = {f(hp)}; p.reverbLpHz = {f(lp)}; p.reverbPre = {f(pre)}; p.reverbPreMix = {f(premix)};")


def uses_drive(filter_type):
    """Drv sets the saturator of the ladder ("-A") types and the gain of the waveshapers (04-filters.md)."""
    return filter_type.endswith("-A") or filter_type.startswith("WS")


# FM connections in XML order -> the engine's index (kFmConns: O11 O12 O13 O22 O23 O1O O2O O3O).
FM_CONNS = {"O11": 0, "O12": 1, "O13": 2, "O22": 3, "O23": 4, "O1O": 5, "O2O": 6, "O3O": 7}


def fm_waves():
    """FM operator waveform names in the order of fm_waves.h (fmwaves2h.py)."""
    src = open(os.path.join(FIRMWARE, "fm_waves.h")).read()
    return [w.strip() for w in re.search(r"constexpr int kFmWaves = \d+;\s*//(.*)", src).group(1).split(",")]
ENV_TYPES = {"ADSR": "kEnvADSR", "AD": "kEnvAD", "Bipolar": "kEnvBipolar"}
VEL_SPLITS = {"Layer": "kVelLayer", "Split 2": "kVelSplit2", "Split 3": "kVelSplit3"}
LFO_TYPES = {"Sine": "kLfoSine", "Square": "kLfoSquare", "Triangle": "kLfoTriangle", "Saw Up": "kLfoSawUp",
             "Saw Down": "kLfoSawDown", "Saw Double": "kLfoSawDouble", "Random": "kLfoRandom",
             "Random Loop": "kLfoRandomLoop"}
LFO_OUTS = {"BIPOLAR": "kLfoBipolar", "UNI HALF": "kLfoUniHalf", "UNI FULL": "kLfoUniFull"}
LFO_SYNCS = {"OFF": "kLfoFree", "KEY": "kLfoKey", "GLOBAL": "kLfoGlobal"}
TEMPO_BPM = 120.0  # the engine has no transport: tempo-synced LFO rates assume this tempo

# Envelopes in matrix order: (XML tag, Patch field).
ENVS = [("EnvA", "ampEnv"), ("EnvF", "filtEnv"), ("EnvP", "pitchEnv"), ("Env4", "env4"), ("Env5", "env5")]
ENV_NAMES = ["Amp Env", "Filter Env", "Pitch Env", "Env 4", "Env 5"]
MACRO_NAMES = [f"Knob {i + 1}" for i in range(8)] + ["Pad X", "Pad Y"]
SOURCES = {"Const One": "kSrcConst", "Rand 1": "kSrcRand1", "Rand 2": "kSrcRand2", "Note": "kSrcNote",
           "Velocity": "kSrcVelocity", "Gate": "kSrcGate", "Pitch Wheel": "kSrcPitchWheel",
           "Pitch Wheel +": "kSrcPitchWheelUp", "Pitch Wheel -": "kSrcPitchWheelDown"}
SOURCES.update({n: f"srcEnv({i})" for i, n in enumerate(ENV_NAMES)})
SOURCES.update({f"LFO {i + 1}": f"srcLfo({i})" for i in range(5)})
SOURCES.update({n: f"srcMacro({i})" for i, n in enumerate(MACRO_NAMES)})
OSC_INDEX = {"Osc 1": 0, "Osc 2": 1, "Osc 3": 2, "Osc All": 3}
OSC_PARAMS = {"Pitch Coarse": "kDstOscPitch", "Pitch Fine": "kDstOscFine", "Level": "kDstOscLevel",
              "Ring Mod": "kDstOscRing", "WT Position": "kDstWtPos", "Noise Cutoff": "kDstNoiseCut",
              "Noise Q": "kDstNoiseQ", "Noise Decimate": "kDstNoiseDeci",
              "FM OP1 FB": "dstFmConn(0)", "FM OP1->OP2": "dstFmConn(1)", "FM OP1->OP3": "dstFmConn(2)",
              "FM OP2 FB": "dstFmConn(3)", "FM OP2->OP3": "dstFmConn(4)", "FM OP1 Out": "dstFmConn(5)",
              "FM OP2 Out": "dstFmConn(6)", "FM OP3 Out": "dstFmConn(7)", "FM OP1 Freq": "dstFmRatio(0)",
              "FM OP2 Freq": "dstFmRatio(1)", "FM OP3 Freq": "dstFmRatio(2)",
              "NanoSaw Detune": "kDstNsDetune", "NanoSaw Mix": "kDstNsMix", "PD Phase Dist": "kDstPdDist",
              "An Pulse Width": "kDstPulseWidth"}
PD_WAVES = {"Saw": "kPdSaw", "Square": "kPdSquare", "Pulse": "kPdPulse", "Double Sine": "kPdDoubleSine",
            "Saw Pulse": "kPdSawPulse", "Res Saw": "kPdResSaw", "Res Fat Saw": "kPdResFatSaw",
            "Res Tri": "kPdResTri", "Res Tri HP": "kPdResTriHP"}
FILTER_PARAMS = {"Cutoff": "kDstCutoff", "Cutoff Slow": "kDstCutoffSlow", "Q": "kDstQ", "Output Gain": "kDstFilterGain",
                 "Drive": "kDstDrive"}
FILTER2_PARAMS = {"Cutoff": "kDstCutoff2", "Q": "kDstQ2", "Output Gain": "kDstFilterGain2", "Drive": "kDstDrive2"}
ROUTES = {"PARALLEL": "kRouteParallel", "SERIES": "kRouteSeries", "STEREO": "kRouteStereo"}
ENV_PARAMS = {"Attack": "kDstEnvAttack", "Decay": "kDstEnvDecay", "Sustain": "kDstEnvSustain",
              "Release": "kDstEnvRelease", "Level": "kDstEnvLevel", "Retrigger": "kDstEnvRetrigger"}
LFO_PARAMS = {"Rate": "kDstLfoRate", "Level": "kDstLfoLevel"}


def engine_constant(name):
    src = open(os.path.join(FIRMWARE, "synth.h")).read()
    return int(re.search(rf"constexpr int {name} = (\d+);", src).group(1))


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "Package.obs")
    return ET.parse(path).getroot()


def num(v):
    """A float attribute; INF (release that never ends) becomes 1e6 seconds."""
    return 1e6 if v == "INF" else float(v)


def f(x):
    """C++ float literal, shortest form that round-trips the XML's 6 significant digits."""
    s = f"{x:.6g}"
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def c_ident(name):
    return "".join(w[:1].upper() + w[1:] for w in re.split(r"[^A-Za-z0-9]+", name))


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Converter:
    def __init__(self, root, name):
        self.root = root
        self.name = c_ident(name)
        self.samples = []  # Zone File names the sample image must hold
        self.voice = root.find("Voice")
        self.code = []
        self.report = []
        self.tables = {}  # WT name -> C++ symbol

    def emit(self, line):
        self.code.append(line)

    def unsupported(self, where, what):
        self.report.append(f"{where}: {what}")

    def check_default(self, where, el, attr, neutral, why=""):
        v = el.get(attr)
        if v is not None and v != neutral:
            self.unsupported(where, f"{attr}={v} not mapped{why}")

    # -- Oscillators ------------------------------------------------------------------------
    def table_symbol(self, name):
        if name not in self.tables:
            self.tables[name] = c_ident(name.split("/")[-1])
        return self.tables[name]

    def osc(self, n):
        o = self.voice.find(f"Osc/Osc{n}")
        where = f"Osc{n} (Osc {n + 1})"
        typ = o.get("Type")
        enabled = o.get("En") == "On"
        q = f"p.osc[{n}]"
        if enabled and typ not in ("Analog", "Wavetable", "Noise", "Sample", "FM", "NanoSaw", "PD"):
            self.unsupported(where, f"Type={typ} not supported, oscillator disabled")
            enabled = False
        self.emit(f"{q}.enabled = {'true' if enabled else 'false'};")
        if not enabled:
            return
        common = (f"{q}.transpose = {int(float(o.get('Trn')))}; {q}.fine = {f(num(o.get('Dtn')))}; "
                  f"{q}.level = {f(num(o.get('Lev')))}; {q}.ring = {f(num(o.get('Ring')))};")
        if typ == "Analog":
            a = o.find("Anlg")
            wave = a.get("Wave")
            if wave not in ANALOG_WAVES:
                self.unsupported(where, f"Analog Wave={wave} not supported, using Square (PWM={a.get('PWM')} ignored)")
            self.emit(f"{q}.type = kOscWave; {q}.wave = {ANALOG_WAVES.get(wave, 'kSquare')};"
                      + (f" {q}.pulseWidth = {f(num(a.get('PWM')))};" if wave == "Pulse" else ""))
            if a.get("Sync", "OFF").upper() != "OFF":  # older presets write "Off"
                # Not a plain hard sync: NS2 at SRat 0.5 keeps the fundamental and adds highs (10-porting-notes.md).
                self.unsupported(where, f"Sync={a.get('Sync')} not mapped")
        elif typ == "Wavetable":
            w = o.find("Wave")
            sym = self.table_symbol(w.find("WT").get("Name"))
            self.emit(f"{q}.type = kOscTable; {q}.table = &k{sym}; {q}.position = {f(num(w.get('Pos')))};")
            if w.get("Mode") != "Crossfade":
                self.unsupported(where, f"Wavetable Mode={w.get('Mode')} not supported, played as Crossfade")
        elif typ == "FM":
            self.fm(o.find("FM"), where, q)
        elif typ == "NanoSaw":
            ns = o.find("NS")
            self.emit(f"{q}.type = kOscNanoSaw; {q}.nsMix = {f(num(ns.get('Mix')))}; {q}.nsFalloff = {f(num(ns.get('FlOf')))}; "
                      f"{q}.nsDetune = {f(num(ns.get('Dtn')))}; {q}.nsSpread = {f(num(ns.get('Sprd')))};")
            self.check_default(where, o, "Widt", "0.5", " (NanoSaw stereo width)")
        elif typ == "PD":
            pd = o.find("PD")
            if pd.get("Wave") not in PD_WAVES:
                self.unsupported(where, f"PD Wave={pd.get('Wave')} not supported, using Saw")
            self.emit(f"{q}.type = kOscPD; {q}.pdWave = {PD_WAVES.get(pd.get('Wave'), 'kPdSaw')}; "
                      f"{q}.pdAmount = {f(num(pd.get('PD')))};")
        elif typ == "Sample":
            self.sample_zones(n, o.find("Samp/Zones"), where, q)
        else:
            z = o.find("Nois")
            ntype = z.get("Type")
            if ntype not in NOISE_TYPES:
                self.unsupported(where, f"Noise Type={ntype} not supported, using White")
            self.emit(f"{q}.type = kOscNoise; {q}.noiseType = {NOISE_TYPES.get(ntype, 'kNoiseWhite')}; "
                      f"{q}.noiseCut = {f(num(z.get('Cut')))}; {q}.noiseQ = {f(num(z.get('Q')))}; "
                      f"{q}.noiseDeci = {f(num(z.get('Deci')))};")
            self.check_default(where, o, "Widt", "0", " (stereo noise)")
        self.emit(common)
        if n == 0:
            self.check_default(where, o, "Ring", "0", " (Osc 1 has no previous oscillator)")
        if num(o.get("KeyT")) != 1.0:
            self.emit(f"{q}.keyTrack = {f(num(o.get('KeyT')))};")
        self.check_default(where, o, "Pan", "0.5")
        if num(o.get("FMix")) != 0.0:
            if self.f2_split:
                self.emit(f"{q}.filterMix = {f(num(o.get('FMix')))};")
        if num(o.get("POff")) != 0.0:
            self.emit(f"{q}.phaseOffset = {f(num(o.get('POff')))};")

    def sample_zones(self, n, zones, where, q):
        sym = f"k{self.name}Osc{n + 1}Zones"
        rows, ranges = [], []
        for z in zones:
            name = z.get("File")
            if not name:
                continue
            if name.startswith("/"):
                self.unsupported(where, f"zone {name}: app resource, not in the preset folder, skipped")
                continue
            lo, hi, root = (int(float(z.get(a))) for a in ("Lo", "Hi", "Root"))
            loop = z.get("Loop")
            if loop == "Spectral":
                self.unsupported(where, f"zone {name}: Loop=Spectral played with the WAV's smpl loop")
            if z.get("Dir") != "Forward":
                self.unsupported(where, f"zone {name}: Dir={z.get('Dir')} not supported, played Forward")
            self.check_default(f"{where} zone {name}", z, "Pan", "0.5")
            if any(lo <= h and l <= hi for l, h in ranges):
                self.unsupported(where, f"zone {name}: overlaps another zone; no zone crossfade, the first zone plays")
            ranges.append((lo, hi))
            if name not in self.samples:
                self.samples.append(name)
            rows.append(f"    {{{c_string(name)}, {lo}, {hi}, {root}, {f(num(z.get('Tune')))}, {f(num(z.get('Vol')))}, "
                        f"{'false' if loop == 'Off' else 'true'}, nullptr}},")
        if not rows:
            self.unsupported(where, "Sample oscillator without zones, silent")
            self.emit(f"{q}.type = kOscSample; {q}.zoneCount = 0;")
            return
        self.emit(f"static SampleZone {sym}[] = {{")
        self.code.extend(rows)
        self.emit("};")
        self.emit(f"{q}.type = kOscSample; {q}.zones = {sym}; {q}.zoneCount = {len(rows)};")

    def osc_block(self):
        b = self.voice.find("Osc")
        t = b.get("Type")
        if t not in VEL_SPLITS:
            self.unsupported("Osc", f"Type={t} not supported, played as Layer")
        self.emit(f"p.velSplit = {VEL_SPLITS.get(t, 'kVelLayer')}; p.velLayer[0] = {f(num(b.get('LL1')))}; "
                  f"p.velLayer[1] = {f(num(b.get('LL2')))}; p.split2 = {f(num(b.get('S2L1')))};")
        self.emit(f"p.split3[0] = {f(num(b.get('S3L1')))}; p.split3[1] = {f(num(b.get('S3L2')))}; "
                  f"p.velXFade[0] = {f(num(b.get('LX1')))}; p.velXFade[1] = {f(num(b.get('LX2')))};")

    def fm(self, fm, where, q):
        names = fm_waves()
        self.emit(f"{q}.type = kOscFM;")
        for k in range(3):
            op = fm.find(f"OP/O{k + 1}")
            wave = op.get("Wave")
            if wave not in names:
                self.unsupported(where, f"FM OP{k + 1} Wave={wave} not supported, using Sine")
            self.emit(f"{q}.fm.wave[{k}] = {names.index(wave) if wave in names else 0}; "
                      f"{q}.fm.ratio[{k}] = {f(int(op.get('Rat')) * 0.0001)}; "
                      f"{q}.fm.freqOffset[{k}] = {f(num(op.get('FrqO')))};  // {wave}")
        for tag, c in FM_CONNS.items():
            conn = fm.find(f"Con/{tag}")
            self.emit(f"{q}.fm.amount[{c}] = {f(num(conn.get('Amt')))};  // {tag}")
            self.env_element(conn.find("Env"), f"{q}.fm.env[{c}]", f"{where} {tag}")

    @staticmethod
    def type_fields(q, flt):
        """The Drv and FVwl assignments of a filter whose type uses them (NS2 ignores WSGn, 04-filters.md)."""
        t = flt.get("Type")
        return ((f" {q}.drive = {f(num(flt.get('Drv')))};" if uses_drive(t) else "")
                + (f" {q}.vowel = {f(num(flt.get('FVwl')))};" if t.startswith("Formant") else ""))

    # -- Filters ----------------------------------------------------------------------------
    def filters(self):
        filt = self.voice.find("Filt")
        f1, f2, f3 = filt.find("F1"), filt.find("F2"), filt.find("F3")
        if f1.get("En") != "On":
            self.emit("p.filterType = kFilterOff; p.filterGain = 1.0f;")
        else:
            t = f1.get("Type")
            if t not in FILTER_TYPES:
                self.unsupported("F1", f"Type={t} not supported, using LP 24-A")
            self.emit(f"p.filterType = {FILTER_TYPES.get(t, 'kFilterLP24A')}; p.cutoff = {f(num(f1.get('CutO')))}; "
                      f"p.resonance = {f(num(f1.get('Q')))}; p.keyTrack = {f(num(f1.get('KeyT')))};")
            self.emit(f"p.filterGain = {f(num(f1.get('OGn')))};"
                      + (f" p.shelfGain = {f(num(f1.get('SGan')))};" if t.startswith("EQ") else "")
                      + self.type_fields("p", f1))
        rout = filt.get("Rout")
        self.emit(f"p.route = {ROUTES[rout]}; p.osc1Bypass = {'true' if filt.get('O1BP') == 'FILTER' else 'false'}; "
                  f"p.filterPan = {f(num(f1.get('Pan')))};")
        if self.f2_on:
            self.emit(f"p.f2.enabled = true; p.f2.type = {FILTER_TYPES[f2.get('Type')]}; "
                      f"p.f2.cutoff = {f(num(f2.get('CutO')))}; p.f2.resonance = {f(num(f2.get('Q')))}; "
                      f"p.f2.keyTrack = {f(num(f2.get('KeyT')))};")
            self.emit(f"p.f2.gain = {f(num(f2.get('OGn')))}; p.f2.shelfGain = {f(num(f2.get('SGan')))}; "
                      f"p.f2.pan = {f(num(f2.get('Pan')))};"
                      + self.type_fields("p.f2", f2))
        elif self.f2_split:
            self.emit(f"p.f2.pan = {f(num(f2.get('Pan')))};  // F2 off: its bus passes through")
            if f2.get("En") == "On":
                self.unsupported("F2", f"Type={f2.get('Type')} not supported, passed through")
        if f3.get("En") == "On":
            t = f3.get("Type")
            if t in ("Comb Pos", "Comb Neg"):
                self.emit(f"p.comb.enabled = true; p.comb.negative = {'true' if t == 'Comb Neg' else 'false'}; "
                          f"p.comb.cutoff = {f(num(f3.get('CutO')))}; p.comb.q = {f(num(f3.get('Q')))};")
                self.emit(f"p.comb.mix = {f(num(f3.get('CMix')))};")
                self.check_default("F3", f3, "Drv", "0")
                self.check_default("F3", f3, "OGn", "1")
                self.check_default("F3", f3, "Pan", "0.5")
            elif t in FILTER_TYPES:
                self.emit(f"p.f3.enabled = true; p.f3.type = {FILTER_TYPES[t]}; p.f3.cutoff = {f(num(f3.get('CutO')))}; "
                          f"p.f3.q = {f(num(f3.get('Q')))};")
                self.emit(f"p.f3.gain = {f(num(f3.get('OGn')))}; p.f3.shelfGain = {f(num(f3.get('SGan')))};"
                          + self.type_fields("p.f3", f3))
                self.check_default("F3", f3, "Pan", "0.5")
            else:
                self.unsupported("F3", f"global filter Type={t} not supported")

    # -- Envelopes and voice -----------------------------------------------------------------
    def env(self, tag, field):
        self.env_element(self.voice.find(f"Env/{tag}"), f"p.{field}", tag)

    def env_element(self, e, q, tag):
        t = e.get("Type")
        if t not in ENV_TYPES:
            self.unsupported(tag, f"Type={t} not supported, played as ADSR")
        self.emit(f"{q}.type = {ENV_TYPES.get(t, 'kEnvADSR')};"
                  + (f" {q}.startLevel = {f(num(e.get('SLv')))}; {q}.releaseLevel = {f(num(e.get('RLv')))};"
                     if t == "Bipolar" else ""))
        self.emit(f"{q}.attack = {f(num(e.get('A')))}; {q}.decay = {f(num(e.get('D')))}; "
                  f"{q}.sustain = {f(num(e.get('S')))}; {q}.release = {f(num(e.get('R')))};")
        self.emit(f"{q}.ac = {f(num(e.get('AC')))}; {q}.dc = {f(num(e.get('DC')))}; {q}.rc = {f(num(e.get('RC')))}; "
                  f"{q}.level = {f(num(e.get('Levl')))};")
        self.emit(f"{q}.vTim = {f(num(e.get('VTim')))}; {q}.vLev = {f(num(e.get('VLev')))}; "
                  f"{q}.kCtr = {int(float(e.get('KCtr')))}; {q}.kTim = {f(num(e.get('KTim')))}; "
                  f"{q}.kLev = {f(num(e.get('KLev')))};")
        for attr in ("Dly", "Brk", "D2"):
            self.check_default(tag, e, attr, "0")

    def voice_params(self):
        v = self.voice
        poly = int(float(v.get("Poly")))
        uni = int(float(v.get("Uni")))
        voices = engine_constant("kVoices")
        # Poly=0 is mono: in NS2 a chord on Deep Bass (Poly=0) sounds one note [P].
        if poly <= 1:
            self.emit(f"p.mono = true; p.glide = {f(num(v.get('GTim')))};")
            self.check_default("Voice", v, "GLeg", "On", " (mono always glides legato)")
        else:
            self.check_default("Voice", v, "GTim", "0", " (glide only in mono, Poly 0 or 1)")
            if poly > voices:
                self.unsupported("Voice", f"Poly={poly}: the engine has {voices} voices")
        self.emit(f"p.unison = {uni}; p.unisonDetune = {f(num(v.get('UDtn')))}; "
                  f"p.unisonStereo = {f(num(v.get('USte')))};")

    # -- Mod matrix and LFOs ----------------------------------------------------------------
    def lfo(self, i):
        l = self.voice.find(f"LFO/LFO{i + 1}")
        where, q = f"LFO{i + 1}", f"p.lfo[{i}]"
        t = l.get("Type")
        if t not in LFO_TYPES:
            self.unsupported(where, f"Type={t} not supported, played as Sine")
        rate = num(l.get("Rt"))
        if l.get("BRt") != "Hz":
            rate = float(l.get("BRt")) / (num(l.get("RtBt")) * 4 * 60.0 / TEMPO_BPM)
            self.unsupported(where, f"tempo rate {l.get('RtBt')}/{l.get('BRt')} played at {TEMPO_BPM:g} BPM")
        sync = l.get("Sync", "OFF").upper()
        if sync not in LFO_SYNCS:
            self.unsupported(where, f"Sync={sync} not supported, free")
        self.emit(f"{q}.rate = {f(rate)}; {q}.ampl = {f(num(l.get('Ampl')))}; {q}.type = {LFO_TYPES.get(t, 'kLfoSine')}; "
                  f"{q}.out = {LFO_OUTS.get(l.get('OutT'), 'kLfoBipolar')}; {q}.sync = {LFO_SYNCS.get(sync, 'kLfoFree')};")
        self.emit(f"{q}.phaseOffset = {f(num(l.get('PhOf')))}; {q}.attack = {f(num(l.get('Atk')))}; "
                  f"{q}.curve = {f(num(l.get('Crv')))}; {q}.warp = {f(num(l.get('Warp')))}; "
                  f"{q}.quantize = {int(float(l.get('Qtz')))}; {q}.loopPattern = {int(float(l.get('RPtn')))}; "
                  f"{q}.loopLength = {int(float(l.get('RLen')))};")
        if t not in ("Sine",) and num(l.get("Crv")) != 0.5 and t in LFO_TYPES:
            self.unsupported(where, f"Crv={l.get('Crv')} only shapes the sine")

    def destinations(self, dest, par):
        """[(ModDest, index)] of a Dest/DstP pair; empty when the engine has no such parameter."""
        if dest in ("Filter 1", "Filter 2", "Filter 1+2"):
            out = []
            if dest != "Filter 2" and par in FILTER_PARAMS:
                out.append((FILTER_PARAMS[par], 0))
            if dest != "Filter 1" and par in FILTER2_PARAMS and self.f2_on:
                out.append((FILTER2_PARAMS[par], 0))
            return out
        target = self.destination(dest, par)
        return [target] if target else []

    def destination(self, dest, par):
        """(ModDest, index) of a non-filter Dest/DstP pair, or None when the engine has no such parameter."""
        if dest in OSC_INDEX and par in OSC_PARAMS:
            return OSC_PARAMS[par], OSC_INDEX[dest]
        # Global Cutoff Slow plays as Cutoff, without the slew [I]: it is set from knobs and pads.
        if dest == "Global Filter" and par in ("Cutoff", "Cutoff Slow", "Q") and self.global_filter:
            return ("kDstGlobalQ" if par == "Q" else "kDstGlobalCutoff"), 0
        if dest == "Amp" and par == "Gain":
            return "kDstAmpGain", 0
        if dest in ENV_NAMES and par in ENV_PARAMS:
            return ENV_PARAMS[par], ENV_NAMES.index(dest)
        if dest.startswith("LFO ") and par in LFO_PARAMS:
            return LFO_PARAMS[par], int(dest[4:]) - 1
        if dest == "FX" and par == "Reverb Mix" and self.reverb_on:
            return "kDstReverbMix", 0
        if dest == "FX" and par == "Multi-FX Mix" and self.phaser_on:
            return "kDstMultiFxMix", 0
        if dest == "FX" and par == "Delay Mix" and self.delay_on:
            return "kDstDelayMix", 0
        return None

    def mod_matrix(self):
        max_slots = engine_constant("kModSlots")
        slots, env_amt, env_amt2 = [], 0.0, 0.0
        for c in self.voice.find("Mod"):
            if not re.fullmatch(r"C\d\d", c.tag) or c.get("Src") == "None":
                continue
            src, dest, par, sca = c.get("Src"), c.get("Dest"), c.get("DstP"), c.get("Sca")
            depth = int(c.get("Dept")) / 100.0
            desc = f"{src} -> {dest} {par}, Dept {c.get('Dept')}" + (f" x {sca}" if sca != "None" else "")
            if depth == 0.0:
                continue
            if src == "Filter Env" and dest.startswith("Filter ") and par == "Cutoff" and sca == "None":
                env_amt += depth if dest != "Filter 2" else 0.0
                env_amt2 += depth if dest != "Filter 1" and self.f2_on else 0.0
                if dest == "Filter 2" and not self.f2_on:
                    self.unsupported(c.tag, f"{desc}: F2 is not running")
                continue
            targets = self.destinations(dest, par)
            if src not in SOURCES or (sca != "None" and sca not in SOURCES) or not targets:
                self.unsupported(c.tag, f"{desc}: not supported")
                continue
            for target in targets:
                slots.append((SOURCES[src], SOURCES.get(sca, "kSrcNone"), target[0], target[1], depth, c.tag, desc))

        self.emit(f"p.filterEnvAmt = {f(env_amt)};" + (f" p.f2.envAmt = {f(env_amt2)};" if env_amt2 else ""))
        used_envs = {int(m.group(1)) for sl in slots for m in [re.match(r"srcEnv\((\d)\)", sl[0])] if m}
        used_envs |= {sl[3] for sl in slots if sl[2].startswith("kDstEnv")}
        for e in sorted(used_envs - {0, 1}):
            self.env(*ENVS[e])
        used_lfos = {int(m.group(1)) for sl in slots for m in [re.match(r"srcLfo\((\d)\)", sl[0])] if m}
        used_lfos |= {sl[3] for sl in slots if sl[2].startswith("kDstLfo")}
        for i in sorted(used_lfos):
            self.lfo(i)
        # Autm C02..C11 are the control positions in the order of the preset's control names: Pad X,
        # Pad Y, Knob 1..8 (macro[8], macro[9], macro[0..7]).
        autm = self.root.find("Autm")
        if autm is not None:
            order = [8, 9] + list(range(8))
            vals = {m: num(autm.get(f"C{k + 2:02d}", "0")) for k, m in enumerate(order)}
            macros = " ".join(f"p.macro[{m}] = {f(vals[m])};" for m in sorted(vals) if vals[m] != 0.0)
            if macros:
                self.emit(macros + "  // Autm: knob and pad positions")
        self.emit("p.clearMod();")
        for k, (src, sca, dest, index, depth, tag, desc) in enumerate(slots):
            if k >= max_slots:
                self.unsupported(tag, f"{desc}: more than {max_slots} slots, dropped")
                continue
            self.emit(f"p.mod[{k}] = {{{src}, {sca}, {dest}, {index}, {f(depth)}}};  // {tag} {desc}")

    # -- Effects ----------------------------------------------------------------------------
    def effects(self):
        fx = self.root.find("FX")
        on = fx.get("En") == "1"
        mfx, dly, rev = fx.find("MFX"), fx.find("Dly"), fx.find("Rev")
        if on and mfx.get("En") == "1":
            t = mfx.get("Type")
            if t in ("Phaser 1", "Phaser 2"):
                self.emit(f"p.phaser.enabled = true; p.phaser.rate = {f(num(mfx.get('Rat')))}; "
                          f"p.phaser.depth = {f(num(mfx.get('Dep')))}; p.phaser.feedback = {f(num(mfx.get('FB')))};")
                self.emit(f"p.phaser.width = {f(num(mfx.get('Wid')))}; p.phaser.mix = {f(num(mfx.get('Mix')))};")
            elif t in CHORUS_TYPES:
                self.emit(f"p.chorus.enabled = true; p.chorus.type = {CHORUS_TYPES[t]}; p.chorus.rate = {f(num(mfx.get('Rat')))}; "
                          f"p.chorus.depth = {f(num(mfx.get('Dep')))}; p.chorus.delay = {f(num(mfx.get('Dly')))};")
                self.emit(f"p.chorus.feedback = {f(num(mfx.get('FB')))}; p.chorus.mix = {f(num(mfx.get('Mix')))};")
        if self.delay_on:
            kind, _, ratio = dly.get("Type").partition(" 1:")
            seconds = num(dly.get("DTim"))
            if dly.get("TDen") != "sec":
                seconds = num(dly.get("TNum")) / float(dly.get("TDen")) * 4 * 60.0 / TEMPO_BPM
                self.unsupported("Dly", f"tempo time {dly.get('TNum')}/{dly.get('TDen')} played at {TEMPO_BPM:g} BPM")
            self.emit(f"p.delay.enabled = true; p.delay.type = kDelay{kind}; p.delay.ratio = {int(ratio or 1)}; "
                      f"p.delay.time = {f(seconds)};")
            self.emit(f"p.delay.feedback = {f(num(dly.get('FB')))}; p.delay.eq = {f(num(dly.get('EQCt')))}; "
                      f"p.delay.mix = {f(num(dly.get('Mix')))};")
        mix = num(rev.get("Mix")) if on and rev.get("En") == "1" else 0.0
        self.emit(f"p.reverbMix = {f(mix)};")
        self.emit(reverb_fields(rev))
        if on and fx.get("Ord") != str(0x020100):
            self.unsupported("FX", f"Ord={fx.get('Ord')} not supported (order is Multi-FX -> Reverb)")

    def run(self):
        fx = self.root.find("FX")
        f3 = self.voice.find("Filt/F3")
        self.global_filter = f3.get("En") == "On" and f3.get("Type") in ("Comb Pos", "Comb Neg", *FILTER_TYPES)
        filt = self.voice.find("Filt")
        f2 = filt.find("F2")
        self.f2_split = filt.get("Rout") != "STEREO"  # STEREO runs F1's settings on both buses
        self.f2_on = self.f2_split and f2.get("En") == "On" and f2.get("Type") in FILTER_TYPES
        self.reverb_on = fx.get("En") == "1" and fx.find("Rev").get("En") == "1"
        self.delay_on = fx.get("En") == "1" and fx.find("Dly").get("En") == "1"
        self.phaser_on = fx.get("En") == "1" and fx.find("MFX").get("En") == "1"  # phaser or chorus
        self.osc_block()
        for n in range(3):
            self.osc(n)
        self.filters()
        self.env("EnvA", "ampEnv")
        self.env("EnvF", "filtEnv")
        self.mod_matrix()
        self.voice_params()
        self.effects()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1].rstrip("/")
    name = os.path.basename(path)
    c = Converter(load(path), os.path.splitext(name)[0])
    c.run()
    print(f"// NS2 preset {name}, generated by firmware/tools/obs2preset.py")
    for line in c.code:
        print(line)

    err = sys.stderr
    if c.tables:
        print("Wavetables (each needs a header and a syn::Wavetable in obsidian_esp32.ino):", file=err)
        for wt, sym in c.tables.items():
            header = f"wt_{sym.lower()}.h"
            tbl = os.path.join(WAVETABLES, wt + ".tbl")
            print(f"  {wt}: #include \"{header}\"", file=err)
            print(f"    static const syn::Wavetable k{sym} = {{k{sym}Frames, k{sym}Data, k{sym}Scale}};", file=err)
            if os.path.exists(os.path.join(FIRMWARE, header)):
                print(f"    {header} already exists", file=err)
            elif os.path.exists(tbl):
                print(f"    python3 -I firmware/tools/tbl2h.py \"{tbl}\" {sym} firmware/obsidian_esp32/{header}", file=err)
            else:
                print(f"    {tbl} not found", file=err)
    if c.samples:
        print(f"Samples ({len(c.samples)} files, bound by name from the sample image at preset load):", file=err)
        print(f"  python3 -I firmware/tools/samples2bin.py samples.bin \"{path}\"", file=err)
        print("  needs firmware/tools/partitions_samples.csv as firmware/obsidian_esp32/partitions.csv; flash with", file=err)
        print("  esptool --chip esp32s3 --port <port> write-flash 0x310000 samples.bin", file=err)
    print(f"Not represented ({len(c.report)}):", file=err)
    for line in c.report:
        print(f"  {line}", file=err)


if __name__ == "__main__":
    main()
