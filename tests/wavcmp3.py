#!/usr/bin/env python3
"""Fine analysis of one 10 s window: sub-window (0.25 s) corr and lag drift, regression gain, spectral peak match.
usage: REF_CH=n wavcmp3.py ref.wav test.wav start_s"""
import sys, os, numpy as np
spec_src = open(os.path.join(os.path.dirname(__file__), "wavcmp.py")).read().split("a, ra = load(")[0]
ns = {}; exec(spec_src, ns); load = ns["load"]
a, ra = load(sys.argv[1], int(os.environ.get("REF_CH", "0"))); b, rb = load(sys.argv[2])
s0 = int(float(sys.argv[3]) * ra); W = 10 * ra
A = a[s0:s0+W]; B = b[s0:s0+W]
def best_lag(x, y, maxlag=2000):
    f = np.fft.rfft(x - x.mean(), 2*len(x)) * np.conj(np.fft.rfft(y - y.mean(), 2*len(x))); xc = np.fft.irfft(f)
    lags = np.concatenate([np.arange(0, maxlag), np.arange(-maxlag, 0)]); vals = np.concatenate([xc[:maxlag], xc[-maxlag:]])
    return int(lags[np.argmax(vals)])
lag = best_lag(A, B)
if lag >= 0: A2, B2 = A[lag:], B[:W-lag]
else: A2, B2 = A[:W+lag], B[-lag:]
n = len(A2)
g = np.cov(A2, B2)[0, 1] / np.var(B2)
print(f"window {sys.argv[3]}s lag {lag} corr {np.corrcoef(A2,B2)[0,1]:.3f} regression gain ref/test {g:.3f} (={20*np.log10(g):+.2f} dB)")
sw = ra // 4
print("sub-windows 0.25 s: lag / corr")
row = []
for k in range(0, n - sw, sw * 4):
    x = A2[k:k+sw]; y = B2[k:k+sw]
    if x.std() == 0 or y.std() == 0: row.append("  -  "); continue
    l = best_lag(x, y, 200); 
    if l >= 0: xx, yy = x[l:], y[:sw-l]
    else: xx, yy = x[:sw+l], y[-l:]
    row.append(f"{l:+d}/{np.corrcoef(xx,yy)[0,1]:.2f}")
print("  " + "  ".join(row))
# spectral peaks over the whole window (phase-insensitive)
FA = np.abs(np.fft.rfft(A2 * np.hanning(n))); FB = np.abs(np.fft.rfft(B2 * np.hanning(n)))
freqs = np.fft.rfftfreq(n, 1 / ra)
print(f"magnitude-spectrum corr {np.corrcoef(FA, FB)[0,1]:.3f}")
top = np.argsort(FA)[-12:][::-1]
print("top ref peaks Hz (ref mag / test mag):", ", ".join(f"{freqs[i]:.1f} ({FA[i]/FB[i] if FB[i] else 0:.2f})" for i in sorted(top)))
