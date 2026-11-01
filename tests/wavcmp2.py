#!/usr/bin/env python3
"""Segment comparison: for 10 s windows, best lag, raw corr, low-passed (<4 kHz) corr, RMS envelope corr (20 ms).
usage: REF_CH=n wavcmp2.py ref.wav test.wav [maxsec]"""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import importlib.util
spec = importlib.util.spec_from_file_location("wavcmp", os.path.join(os.path.dirname(__file__), "wavcmp.py"))
src = open(spec.origin).read().split("a, ra = load(")[0]   # reuse load() only
ns = {}; exec(src, ns); load = ns["load"]
a, ra = load(sys.argv[1], int(os.environ.get("REF_CH", "0"))); b, rb = load(sys.argv[2])
n = min(len(a), len(b)); n = min(n, int(float(sys.argv[3]) * ra)) if len(sys.argv) > 3 else n
a = a[:n]; b = b[:n]
def lowpass(x, fc):
    f = np.fft.rfft(x); k = int(fc / ra * len(x)); f[k:] = 0; return np.fft.irfft(f, len(x))
def env(x, w=882):
    m = len(x) // w; return np.sqrt(np.mean(x[:m*w].reshape(m, w) ** 2, axis=1))
def corr(x, y): return np.corrcoef(x, y)[0, 1] if x.std() and y.std() else float("nan")
W = 10 * ra
print(f"{'win':>6} {'lag':>6} {'corr':>7} {'corr<4k':>8} {'envcorr':>8} {'rms ref':>8} {'rms test':>8}")
for s0 in range(0, n - W + 1, W):
    A = a[s0:s0+W]; B = b[s0:s0+W]
    if A.std() == 0 or B.std() == 0: print(f"{s0//ra:>5}s  silent"); continue
    f = np.fft.rfft(A - A.mean(), 2*W) * np.conj(np.fft.rfft(B - B.mean(), 2*W)); xc = np.fft.irfft(f); lag = int(np.argmax(xc)); lag = lag - 2*W if lag > W else lag
    L = min(abs(lag), W // 2)
    if lag >= 0: A2, B2 = A[lag:], B[:W-lag]
    else: A2, B2 = A[:W+lag], B[-lag:]
    print(f"{s0//ra:>5}s {lag:>6} {corr(A2,B2):7.3f} {corr(lowpass(A2,4000),lowpass(B2,4000)):8.3f} {corr(env(A2),env(B2)):8.3f} {np.sqrt(np.mean(A*A)):8.0f} {np.sqrt(np.mean(B*B)):8.0f}")
