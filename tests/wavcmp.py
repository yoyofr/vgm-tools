#!/usr/bin/env python3
"""Compare two WAVs (MAME reference vs vgm2wav render): best lag, correlation, RMS ratio. usage: wavcmp.py ref.wav test.wav"""
import sys, struct, numpy as np
import os
def load(p, force_ch=0):
    d = open(p, "rb").read(); q = 12; ch = 2; bits = 16
    while q + 8 <= len(d):
        tag = d[q:q+4]; n = int.from_bytes(d[q+4:q+8], "little")
        if tag == b"fmt ": ch = struct.unpack_from("<H", d, q+10)[0]; rate = struct.unpack_from("<I", d, q+12)[0]; bits = struct.unpack_from("<H", d, q+22)[0]
        if tag == b"data":
            if n == 0 or q + 8 + n > len(d): n = len(d) - q - 8   # streaming writer: size not patched
            if force_ch: ch = force_ch
            if ch == 0: ch = 1   # MAME -sound none writes 0 channels in the header
            bps = bits // 8; n -= n % (bps * ch)
            a = np.frombuffer(d[q+8:q+8+n], dtype=np.int16 if bits == 16 else np.int32).astype(np.float64)
            return a.reshape(-1, ch).mean(axis=1), rate
        q += 8 + n + (n & 1)
a, ra = load(sys.argv[1], int(os.environ.get("REF_CH", "0"))); b, rb = load(sys.argv[2])
n = min(len(a), len(b)); n = min(n, int(float(sys.argv[3]) * ra)) if len(sys.argv) > 3 else n; a = a[:n]; b = b[:n]
rms = lambda x: np.sqrt(np.mean(x * x)) if len(x) else 0.0
# lag search via FFT cross-correlation on a 20 s window
w = min(n, 20 * ra); A = a[:w] - a[:w].mean(); B = b[:w] - b[:w].mean()
f = np.fft.rfft(A, 2 * w) * np.conj(np.fft.rfft(B, 2 * w)); xc = np.fft.irfft(f); lag = int(np.argmax(xc)); lag = lag - 2 * w if lag > w else lag
if lag >= 0: a2, b2 = a[lag:], b[:n - lag]
else: a2, b2 = a[:n + lag], b[-lag:]
c = np.corrcoef(a2, b2)[0, 1] if rms(a2) and rms(b2) else float("nan")
print(f"rate {ra}/{rb} len {n/ra:.1f}s lag {lag} samples ({lag/ra*1000:.1f} ms) corr {c:.4f} rms ref {rms(a):.0f} test {rms(b):.0f} ratio {rms(b)/rms(a) if rms(a) else 0:.3f} peak ref {np.abs(a).max():.0f} test {np.abs(b).max():.0f}")
