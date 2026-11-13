#!/usr/bin/env python3
"""QA tool: clicks (crackle) in the windows of a downaudio recording (tools/qa_audio.sh).
Usage: wav_window.py <file.wav> <seconds of the down event in the file> [clicks per window only]
Windows: before (down-12 .. down-1), down (down+2 .. down+22: the downed player, hit at +5), after (down+31 .. down+45).
A click is a sample whose second difference is more than 12 times the window's median and above 0.02; samples closer than 20 ms are one click.
"""
import sys
import wave

import numpy as np


def clicks(m, r, a, b):
    s = m[int(a * r):int(b * r)]
    if len(s) < r:
        return None, 0.0
    d2 = np.abs(np.diff(s, 2))
    med = np.median(d2) + 1e-7
    idx = np.nonzero((d2 > 12 * med) & (d2 > 0.02))[0] / r
    n, last = 0, -1.0
    for v in idx:
        if v - last > 0.02:
            n += 1
        last = v
    return n, float(np.sqrt(np.mean(s * s)))


w = wave.open(sys.argv[1])
r = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768
m = x.reshape(-1, 2).mean(axis=1)
d = float(sys.argv[2])
for name, a, b in (("before", d - 12, d - 1), ("down  ", d + 2, d + 22), ("after ", d + 31, d + 45)):
    n, rms = clicks(m, r, a, b)
    print("%s %6.1f..%6.1f s: %s clicks (%.2f per second), rms %.4f" % (name, a, b, n, (n or 0) / (b - a), rms))
