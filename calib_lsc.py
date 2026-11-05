#!/usr/bin/python3
"""Flat-field lens-shading calibration for the IPU7 LBFF LSC table.

Capture: run live2 with LSC_ZERO=1 LIVE_DUMP=<n> while the camera sees an evenly lit, featureless
field (e.g. a sheet of white paper held against the lens under room light). That dumps the raw
(shading-uncorrected) RGBS grid: 60x68 blocks of 32x16 px, Gr,R,B,Gb u16.

Fit: per channel, a 2D polynomial on log(brightness), evaluated at the LSC grid points
(x = 32c, y = 32r; 64 x 35), gain = centre / value, table value = (gain - 1) * 2048.

usage: calib_lsc.py <rgbs.bin> <out_table.bin> [compare_with_table.bin]
"""
import sys
import numpy as np

GW, GH = 60, 68
rgbs = np.fromfile(sys.argv[1], dtype='<u2').astype(float).reshape(GH, GW, 4)

by, bx = np.mgrid[0:GH, 0:GW]
px, py = bx * 32 + 16, by * 16 + 8                 # block centres in pixels
cx, cy = 1928 / 2, 1088 / 2
nx, ny = (px - cx) / cx, (py - cy) / cx            # isotropic normalisation

def terms(x, y, deg=6):
    return np.stack([x ** i * y ** j for i in range(deg + 1) for j in range(deg + 1 - i)], -1)

gx, gy = np.mgrid[0:64, 0:35]
gx, gy = gx.T * 32.0, gy.T * 32.0                  # (35, 64)
gnx, gny = (gx - cx) / cx, (gy - cy) / cx

table = np.zeros((35, 64, 4))
valid = rgbs.min(-1) > 512                          # need signal
for ch in range(4):
    v = rgbs[..., ch]
    A = terms(nx[valid], ny[valid]); b = np.log(v[valid])
    coef, *_ = np.linalg.lstsq(A, b, rcond=None)
    fit = np.exp(terms(gnx, gny) @ coef)
    centre = np.exp(terms(np.array([0.0]), np.array([0.0])) @ coef)[0]
    gain = np.clip(centre / fit, 1.0, 1 + 65535 / 2048)
    # beyond the measured area the 2D polynomial extrapolates badly: continue radially instead
    rad = np.hypot(gnx, gny); edge = 0.98
    sel = (rad > 0.5) & (rad <= edge)
    rc = np.polyfit(rad[sel] ** 2, np.log(gain[sel]), 2)
    out = rad > edge
    gain[out] = np.clip(np.exp(np.polyval(rc, rad[out] ** 2)), 1.0, 1 + 65535 / 2048)
    table[..., ch] = (gain - 1) * 2048
    resid = np.std(np.log(v[valid]) - A @ coef)
    print(f'ch{ch}: fit residual {resid * 100:.2f}%  corner gain {gain[1, 1]:.2f} {gain[1, 59]:.2f} '
          f'{gain[33, 1]:.2f} {gain[33, 59]:.2f}  centre {gain[17, 30]:.2f}')

# colour shading relative to green, reported for sanity
g = 1 + table / 2048
print('R/G gain ratio range %.3f-%.3f, B/G %.3f-%.3f' % (
    (g[..., 1] / g[..., 0]).min(), (g[..., 1] / g[..., 0]).max(), (g[..., 2] / g[..., 0]).min(), (g[..., 2] / g[..., 0]).max()))
np.round(table).astype('<u2').tofile(sys.argv[2])
print('wrote', sys.argv[2])

if len(sys.argv) > 3:
    ref = 1 + np.fromfile(sys.argv[3], dtype='<u2').astype(float).reshape(35, 64, 4) / 2048
    inside = (gx < 1928) & (gy <= 1088)
    for ch, n in enumerate(['Gr', 'R', 'B', 'Gb']):
        d = g[..., ch][inside] / ref[..., ch][inside] - 1
        print(f'vs reference {n}: mean {d.mean() * 100:+.1f}%  abs max {np.abs(d).max() * 100:.1f}%')
