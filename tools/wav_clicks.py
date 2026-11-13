#!/usr/bin/env python3
"""QA tool: counts clicks (crackle) per second in a 16-bit stereo WAV made by tools/loopback_rec.exe.

A click is a sample where the second difference (the sharp, high-frequency part of the signal) is far above its own surroundings:
more than K times the median of the second differences of the same half second, and above an absolute floor. Per second it prints
the RMS level (dBFS), the click count, and the share of 10 ms blocks that were digital silence.
Usage: wav_clicks.py <file.wav> [K, default 12]
"""
import sys
import wave

import numpy as np


def main():
    path = sys.argv[1]
    k = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    x = x.reshape(-1, 2).mean(axis=1)
    d2 = np.abs(np.diff(x, 2))
    win = rate // 2
    total_clicks = 0
    for s in range(0, len(d2) - rate + 1, rate):
        seg = d2[s:s + rate]
        clicks = 0
        for h in (0, 1):
            part = seg[h * win:(h + 1) * win]
            med = np.median(part) + 1e-6
            clicks += int(np.count_nonzero((part > k * med) & (part > 0.02)))
        blk = rate // 100
        xs = x[s:s + rate]
        nb = len(xs) // blk
        silent = float(np.mean(np.max(np.abs(xs[:nb * blk].reshape(nb, blk)), axis=1) < 1e-4)) if nb else 0.0
        rms = float(np.sqrt(np.mean(xs * xs)) + 1e-9)
        total_clicks += clicks
        print("%3d s  rms %6.1f dBFS  clicks %4d  silent blocks %3d%%" % (s // rate, 20 * np.log10(rms), clicks, round(silent * 100)))
    print("total clicks:", total_clicks)


main()
