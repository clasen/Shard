"""rank.py <ns2 dir> <render cmd> <content.bin> [prefix]: renders every NS2 take "<prefix>NN <preset>.raw" with
the port and ranks the presets by how far they are from NS2 (needs numpy).

Per preset: the global gain NS2/port (dB), and after removing it, the mean absolute difference of the
10 ms level envelopes where NS2 is within 40 dB of its peak, and of the 1/3-octave spectra of each note's
first second over the bands within 30 dB of the strongest one (so inaudible residue does not count).
"""
import os, re, subprocess, sys
import numpy as np

SR = 48000


def load(path):
    return np.fromfile(path, dtype=np.float32).astype(np.float64)


def frames(x, n=480):
    k = len(x) // n
    return np.sqrt(np.mean(x[:k * n].reshape(k, n) ** 2, axis=1) + 1e-20)


def align(a, b):
    """Offset (samples) of b's content inside a, from the note onsets, refined to 8 samples."""
    # Coarse: first 10 ms frame within 20 dB of the peak (some takes carry a -35 dB noise floor).
    # An envelope cross-correlation can lock onto a later, identical-looking note.
    ea, eb = frames(a), frames(b)
    onset = lambda e: int(np.argmax(e > e.max() * 0.1))
    coarse = max(onset(ea) - onset(eb), 0) * 480
    seg = slice(coarse, coarse + SR * 4)
    best, off = -1e30, coarse
    for d in range(-480, 481, 8):
        lo = coarse + d
        if lo < 0: continue
        v = np.dot(np.abs(a[lo:lo + SR * 4]), np.abs(b[: SR * 4]))
        if v > best: best, off = v, lo
    return off


def bands(x):
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / SR)
    edges = 25 * 2 ** (np.arange(0, 30) / 3)
    out = np.array([spec[(f >= lo) & (f < hi)].sum() for lo, hi in zip(edges[:-1], edges[1:])])
    return 10 * np.log10(out / (out.sum() + 1e-30) + 1e-12)


def compare(ns, po):
    off = align(ns, po)
    n = min(len(ns) - off, len(po))
    a, b = ns[off:off + n], po[:n]
    g = 20 * np.log10(np.sqrt(np.mean(a ** 2)) / np.sqrt(np.mean(b ** 2) + 1e-20) + 1e-20)
    b = b * 10 ** (g / 20)
    ea, eb = 20 * np.log10(frames(a)), 20 * np.log10(frames(b))
    loud = ea > ea.max() - 40
    env = float(np.mean(np.abs(ea - np.maximum(eb, ea.max() - 60))[loud]))
    spec = []
    for t in range(0, 20, 2):
        s = slice(int((t + 0.05) * SR), int((t + 1.05) * SR))
        if s.stop > n: break
        ba, bb = bands(a[s]), bands(b[s])
        m = ba > ba.max() - 30
        if m.any(): spec.append(np.mean(np.abs(ba - bb)[m]))
    return g, env, float(np.mean(spec)) if spec else float("nan")


def main():
    nsdir, render, image = sys.argv[1:4]
    prefix = sys.argv[4] if len(sys.argv) > 4 else "zs"
    rows = []
    for fn in sorted(os.listdir(nsdir)):
        m = re.match(re.escape(prefix) + r"\d+ (.+)\.raw$", fn)
        if not m: continue
        out = "/tmp/rank_port.raw"
        # The content image names a preset after its .obs file: factory presets copied as
        # "<prefix>NN <preset>" keep the plain name, generated variants keep the prefix.
        name = m.group(1)
        if subprocess.run([render, image, name, out], capture_output=True).returncode:
            name = fn[:-4]
            if subprocess.run([render, image, name, out], capture_output=True).returncode: continue
        g, e, s = compare(load(os.path.join(nsdir, fn)), load(out))
        rows.append((e + s, name, g, e, s))
    print("%-22s %8s %8s %8s" % ("preset", "gain dB", "env dB", "spec dB"))
    for _, name, g, e, s in sorted(rows, reverse=True):
        print("%-22s %8.1f %8.1f %8.1f" % (name, g, e, s))


if __name__ == "__main__":
    main()
