#!/usr/bin/env python3
"""pack_vac_catalog.py — pack PolyPhy's bundled VAC input catalog into
Polyphorm's .bin/_metadata.txt input pair (M5 validation dataset).

Input : ~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv
        (READ-ONLY; 324,901 rows, no header, columns x,y,z,weight;
        xyz in Mpc, weight in 1e9 Msun — exactly 1000x Polyphorm's
        1e12 Msun bin convention, per rhizome DATA_LINEAGE.md)
Output: <out>.bin           float32 XYZW records (arr.tofile)
        <out>_metadata.txt  the exact positional key sequence
                            pack_data_celestial.py writes — main.cpp's
                            metadata parser is positional, order matters.

Offline only (numpy, no network). Mirrors pack_data_celestial.py's output
conventions; the shipped 37k demo slice is untouched.
"""
import argparse
import os
import sys

import numpy as np

CSV_DEFAULT = os.path.expanduser(
    '~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv')
# The VAC's own dataset name (reference export_metadata.txt) so the run's
# --dataset line matches the published metadata verbatim.
OUT_DEFAULT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'bin', 'data', 'SDSS',
    'sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0'))

EXPECTED_POINTS = 324901           # validation-target research §3
WEIGHT_DIVISOR = 1000.0            # 1e9 Msun (CSV) -> 1e12 Msun (Polyphorm)
SHIPPED_MEAN_WEIGHT = 0.013950215  # shipped galaxiesInSdssSlice metadata —
                                   # same sample family, so a 1000x unit slip
                                   # is unmistakable (design §5.3)


def load_catalog(csv_path):
    """(N, 4) float32 [x, y, z, weight_1e12Msun]; hard-fails on unit slips."""
    data = np.loadtxt(csv_path, delimiter=',')            # (N, 4) float64
    if data.ndim != 2 or data.shape[1] != 4:
        sys.exit(f'FATAL: expected 4 columns, got shape {data.shape}')
    if data.shape[0] != EXPECTED_POINTS:
        sys.exit(f'FATAL: expected {EXPECTED_POINTS} points, got {data.shape[0]}')
    data[:, 3] /= WEIGHT_DIVISOR
    if not (data[:, 3] > 0).all():
        sys.exit('FATAL: non-positive weights after conversion')
    mean_w = float(data[:, 3].mean())
    if not (SHIPPED_MEAN_WEIGHT / 10.0 < mean_w < SHIPPED_MEAN_WEIGHT * 10.0):
        sys.exit(f'FATAL: mean converted weight {mean_w} not within one order '
                 f'of magnitude of shipped {SHIPPED_MEAN_WEIGHT} — unit slip?')
    # float32 at the end, matching the upstream packer's np.float32 output.
    return data.astype(np.float32)


def pack(data, out_base):
    """Write <out_base>.bin + <out_base>_metadata.txt (positional keys)."""
    with open(out_base + '_metadata.txt', 'w') as f:
        f.write('Number of points = ' + str(data.shape[0]) + '\n')
        f.write('Min X = ' + str(np.min(data[:, 0])) + '\n')
        f.write('Max X = ' + str(np.max(data[:, 0])) + '\n')
        f.write('Min Y = ' + str(np.min(data[:, 1])) + '\n')
        f.write('Max Y = ' + str(np.max(data[:, 1])) + '\n')
        f.write('Min Z = ' + str(np.min(data[:, 2])) + '\n')
        f.write('Max Z = ' + str(np.max(data[:, 2])) + '\n')
        f.write('Mean weight = ' + str(np.mean(data[:, 3])) + '\n')
    data.tofile(out_base + '.bin')

    bbox_min = data[:, :3].min(axis=0)
    bbox_max = data[:, :3].max(axis=0)
    mid = 0.5 * (bbox_min + bbox_max)
    print(f'packed {data.shape[0]} points -> {out_base}.bin '
          f'({os.path.getsize(out_base + ".bin")} bytes)')
    print(f'bbox min {bbox_min}, max {bbox_max}')
    print(f'bbox midpoint {mid}  (VAC grid center: -239.469, -16.5618, 201.275)')
    print(f'mean weight {np.mean(data[:, 3])}  '
          f'(shipped-slice reference: {SHIPPED_MEAN_WEIGHT})')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--csv', default=CSV_DEFAULT)
    ap.add_argument('--out', default=OUT_DEFAULT,
                    help='output base path (no extension)')
    args = ap.parse_args()
    data = load_catalog(args.csv)
    pack(data, args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
