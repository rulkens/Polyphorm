#!/usr/bin/env python3
"""pack_vac_catalog.py — pack PolyPhy's bundled VAC input catalog into
Polyphorm's .bin/_metadata.txt input pair (M5 validation dataset).

Input : bin/data/reference/sample_3D_linW.csv (vendored from PolyPhy;
        provenance in bin/data/reference/README.md. 324,901 rows, no
        header, columns x,y,z,weight; xyz in Mpc, weight in 1e9 Msun —
        exactly 1000x Polyphorm's 1e12 Msun bin convention)
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

REPO_ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..'))
CSV_DEFAULT = os.path.join(REPO_ROOT, 'bin', 'data', 'reference',
                           'sample_3D_linW.csv')
# The VAC's own dataset name (reference export_metadata.txt) so the run's
# --dataset line matches the published metadata verbatim.
OUT_DEFAULT = os.path.join(
    REPO_ROOT, 'bin', 'data', 'SDSS',
    'sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0')

EXPECTED_POINTS = 324901           # validation-target research §3
WEIGHT_DIVISOR = 1000.0            # 1e9 Msun (CSV) -> 1e12 Msun (Polyphorm)
SHIPPED_MEAN_WEIGHT = 0.013950215  # shipped galaxiesInSdssSlice metadata —
                                   # same sample family, so a 1000x unit slip
                                   # is unmistakable (design §5.3)

# Published VAC grid (bin/data/reference/export_metadata.txt) — the anchoring
# target. Strategy (design §6.1): do NOT fork main.cpp's quirk-preserved
# auto-fit; feed it the same inputs (this catalog's bbox + config knobs) and
# predict what it will produce. The C++ is authoritative; the run's own
# export_metadata.txt is the ground-truth check.
VAC_DIMS = (712, 1200, 728)
VAC_SIZE = (556.288, 937.564, 568.789)
VAC_CENTER = (-239.469, -16.5618, 201.275)
VAC_VOXEL = 0.78131  # Mpc, isotropic


def nearest_multiple_of(n, m):
    """main.cpp:220-224 verbatim, integer arithmetic."""
    r = (n - 1) % m + 1
    return n + (m - r)


def predict_grid(bbox_min, bbox_max, resolution, padding):
    """Read-only mirror of main.cpp:418-442 (bbox -> pad by padding*max_extent
    -> per-axis scale of `resolution` -> nearest multiple of 8 -> cubic-voxel
    rescale of Y/Z world sizes). np.float32 throughout to mirror the C++
    float arithmetic, including the int() truncation before rounding to 8.
    Returns (dims[3] int, world_size[3] f32, center[3] f32)."""
    bbox_min = bbox_min.astype(np.float32)
    bbox_max = bbox_max.astype(np.float32)
    size = bbox_max - bbox_min
    wmax = np.float32(size.max())
    size = size + np.float32(padding) * wmax
    wmax = np.float32(size.max())
    dims = np.array([nearest_multiple_of(int(np.float32(resolution) * (s / wmax)), 8)
                     for s in size], dtype=np.int64)
    size_out = size.copy()
    size_out[1] = np.float32(dims[1]) * size[0] / np.float32(dims[0])
    size_out[2] = np.float32(dims[2]) * size[0] / np.float32(dims[0])
    center = np.float32(0.5) * (bbox_min + bbox_max)
    return dims, size_out, center


def verify_grid(data, resolution, padding):
    """Predictor + provenance checks. Exit 0 = prediction matches the VAC
    grid; exit 1 = mismatch (message names the remedy)."""
    bbox_min = data[:, :3].min(axis=0).astype(np.float64)
    bbox_max = data[:, :3].max(axis=0).astype(np.float64)
    ext = bbox_max - bbox_min
    mid = 0.5 * (bbox_min + bbox_max)

    # 1. bbox midpoint must equal the VAC grid center to sub-voxel — the fit
    #    centers on the midpoint and padding doesn't move it. A miss means
    #    this catalog is NOT the VAC input: stop and escalate (design §6.1).
    dmid = np.abs(mid - np.array(VAC_CENTER))
    print(f'bbox midpoint {mid}')
    print(f'VAC center    {VAC_CENTER}   |delta| {dmid} Mpc (voxel {VAC_VOXEL})')
    if (dmid >= VAC_VOXEL).any():
        sys.exit('FATAL: bbox midpoint does not match the VAC grid center to '
                 'sub-voxel — this catalog is not the VAC input. STOP and '
                 'escalate to the human (design §6.1 step 1).')

    # 2. Back-solve the padding: p = (published_size - extent) / max_extent.
    #    X is exact; Y/Z published sizes are post-cubic-rescale so those two
    #    are approximate (rounding-to-8 effects). All three ~= 0.1 is a free
    #    provenance check that the VAC ran at GRID_PADDING = 0.1.
    p = (np.array(VAC_SIZE) - ext) / ext.max()
    print(f'back-solved GRID_PADDING per axis: {p}  (config.polyp default 0.1; '
          f'X exact, Y/Z approximate post-rescale)')

    # 3. Predict the C++ fit at the requested knobs.
    dims, size, center = predict_grid(bbox_min, bbox_max, resolution, padding)
    print(f'predicted dims @ resolution {resolution}, padding {padding}: '
          f'{tuple(int(d) for d in dims)}   (VAC: {VAC_DIMS})')
    print(f'predicted world size: {size}   (VAC: {VAC_SIZE})')
    expected_dims = VAC_DIMS if resolution == 1200 else \
        tuple(d // (1200 // resolution) for d in VAC_DIMS) if 1200 % resolution == 0 else None
    if tuple(int(d) for d in dims) == VAC_DIMS and \
       np.allclose(size, VAC_SIZE, rtol=2e-3):
        print('PREDICTION MATCHES the published VAC grid.')
        return
    if resolution != 1200:
        # Fallback-resolution runs (e.g. 600 -> expect 356x600x364) are
        # checked by eye against the printed prediction; only the native
        # resolution is asserted against VAC_DIMS.
        print(f'(non-native resolution {resolution}: expected roughly '
              f'{expected_dims}; empirical check is the --headless run)')
        return
    sys.exit('MISMATCH at native resolution. First remedy: set config.polyp '
             f'"Grid padding" to the back-solved X value {p[0]:.6f} and re-run '
             '--verify-grid (documented config choice, no code change). If NO '
             'single padding value reproduces the grid, STOP and escalate to '
             'the human with the residual in voxels — forking the quirk-'
             'preserved C++ fit needs explicit sign-off (design §6.1).')


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
    ap.add_argument('--verify-grid', action='store_true',
                    help='predict the C++ grid fit instead of packing (read-only)')
    ap.add_argument('--resolution', type=int, default=1200,
                    help='GRID_RESOLUTION knob to predict with (600 = fallback)')
    ap.add_argument('--padding', type=float, default=0.1,
                    help='GRID_PADDING knob to predict with')
    args = ap.parse_args()
    data = load_catalog(args.csv)
    if args.verify_grid:
        verify_grid(data, args.resolution, args.padding)
    else:
        pack(data, args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
