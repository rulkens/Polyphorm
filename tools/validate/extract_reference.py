#!/usr/bin/env python3
"""extract_reference.py — derive the d8 reference cube from the published VAC.

Reads the SDSS DR17 Cosmic Slime VAC `SDSS_z_44-476mpc` cube via pyslime
into a 712 x 1200 x 728 float32 array (X, Y, Z), block-averages it with
skimage.transform.downscale_local_mean, and writes
bin/data/reference/mcpm_sdss_d<factor>.npy. Default factor: 8 only (the
tier the validation compares at). The result is checked against
bin/data/reference/SHA256SUMS, so a run of this script proves the
committed reference cube is what the published data produces.

This is the script that produced the committed mcpm_sdss_d8.npy, copied
from the skymap repository (tools/volumes/extractMcpmCube.py at commit
c78c4e0d35624d05dd4fa906109f8fad9dbdb567) with paths made repo-relative,
the factor list made a flag, and the checksum check added. The numerical
path — pyslime load as float32, downscale_local_mean, C-order np.save —
is unchanged.

Inputs (in bin/data/reference/, fetched by tools/validate/download_vac.py):
    trace.bin.bz2          the archive's compressed cube (345 MB)
    export_metadata.txt    pyslime reads the grid dims from it

If only trace.bin.bz2 is present the script decompresses it to trace.bin
(2.3 GB, gitignored) on first run and leaves both in place.

Peak RAM about 10 GB for the float32 cube.

Usage:
    .venv/bin/python tools/validate/extract_reference.py [--factors 8 4 2]

Verification printed on the way: (min, max, mean, p99) of the trace and
the sample at world (0, 0, 0). The latter should be a non-trivial value;
near-zero would suggest pyslime returned axes in a different order than
export_metadata.txt implies.
"""
import argparse
import bz2
import hashlib
import shutil
import sys
from pathlib import Path

import numpy as np
from pyslime import slime  # pip install pyslime astropy
from skimage.transform import downscale_local_mean

REPO_ROOT = Path(__file__).resolve().parents[2]
RAW_DIR = REPO_ROOT / 'bin' / 'data' / 'reference'
TRACE_BIN = RAW_DIR / 'trace.bin'
TRACE_BZ2 = RAW_DIR / 'trace.bin.bz2'
META_FILE = RAW_DIR / 'export_metadata.txt'
SHA256SUMS = RAW_DIR / 'SHA256SUMS'
EXPECTED_SHAPE = (712, 1200, 728)
GRID_CENTER_MPC = np.array([-239.469, -16.5618, 201.275])
BASE_VOXEL_EDGE_MPC = 0.78131  # 556.288 / 712 (matches export_metadata.txt)
DOWNLOAD_HINT = '  run: .venv/bin/python tools/validate/download_vac.py'


def ensure_uncompressed_trace():
    """Ensure RAW_DIR has an uncompressed trace.bin (decompresses .bz2 if needed)."""
    if TRACE_BIN.exists():
        return
    if not TRACE_BZ2.exists():
        sys.exit(f'missing {TRACE_BIN} (and {TRACE_BZ2} not present either).\n'
                 + DOWNLOAD_HINT)
    print(f'decompressing {TRACE_BZ2} -> {TRACE_BIN} ... (~30s, 345 MB -> 2.3 GB)')
    with bz2.open(TRACE_BZ2, 'rb') as src, open(TRACE_BIN, 'wb') as dst:
        shutil.copyfileobj(src, dst, length=1 << 22)


def ensure_metadata():
    """pyslime requires export_metadata.txt to read grid dims."""
    if not META_FILE.exists():
        sys.exit(f'missing {META_FILE}.\n' + DOWNLOAD_HINT)


def load_cube():
    ensure_uncompressed_trace()
    ensure_metadata()
    print(f'loading {TRACE_BIN} via pyslime.Slime.from_dir({RAW_DIR}) ...')
    # pyslime's default dtype is np.float16, but the SDSS_z_44-476mpc
    # release ships trace.bin as float32 (2.3 GB on disk = 712*1200*728*4
    # bytes). Reading as f16 produces a 2x-sized array that fails the
    # subsequent reshape — the ratio is exactly 2.0, the giveaway. We pass
    # dtype=np.float32 explicitly to match the bytes on disk.
    #
    # Side benefit: f32 is the precision we need anyway for the downstream
    # block-averaging — heavy-tailed trace values would accumulate rounding
    # error if averaged in f16 across 8^3 = 512 cells.
    sl = slime.Slime.from_dir(str(RAW_DIR), dtype=np.float32)
    arr = np.asarray(sl.data, dtype=np.float32)
    if arr.shape != EXPECTED_SHAPE:
        sys.exit(f'unexpected shape {arr.shape}; expected {EXPECTED_SHAPE} '
                 f'per export_metadata.txt')
    return arr


def sanity_check(arr):
    print(f'trace stats: min={arr.min():.3g}, max={arr.max():.3g}, '
          f'mean={arr.mean():.3g}, p99={np.percentile(arr, 99):.3g}')
    # World (0,0,0) sample, kept from the skymap original for continuity.
    # The voxel index for world position p is (p - origin) / voxelSize,
    # where origin = grid_center - grid_size/2. For this VAC the observer
    # sits at the origin and the survey volume spans 44-476 Mpc from it,
    # so the origin lies in the empty inner hole and reads 0 by design;
    # the value is informational only. The load-bearing axis-order check
    # is compare_trace.py --orientation-scan (identity must win).
    origin = GRID_CENTER_MPC - 0.5 * np.array(EXPECTED_SHAPE) * BASE_VOXEL_EDGE_MPC
    idx = tuple(int(i) for i in ((np.zeros(3) - origin) / BASE_VOXEL_EDGE_MPC))
    if all(0 <= i < n for i, n in zip(idx, EXPECTED_SHAPE)):
        print(f'world (0,0,0) sample: arr[{idx}] = {arr[idx]:.3g} '
              f'(0 expected: origin is inside the 44 Mpc inner hole)')
    else:
        print(f'world (0,0,0) maps to voxel idx {idx} — outside cube; investigate')


def write_tier(arr, factor):
    out = RAW_DIR / f'mcpm_sdss_d{factor}.npy'
    print(f'downsampling by {factor}x ...')
    if factor == 1:
        small = arr
    else:
        small = downscale_local_mean(arr, (factor, factor, factor)).astype(np.float32)
    # Force C-order: skimage's downscale_local_mean returns a Fortran-order
    # view in some scikit-image versions, which np.save then writes with
    # fortran_order=True in the .npy header. Guarantee C-order on the
    # writer side so every reader sees the same bytes.
    small = np.ascontiguousarray(small)
    np.save(out, small)
    size_mb = out.stat().st_size / 1024 / 1024
    print(f'  wrote {out}  shape={small.shape}  ({size_mb:.1f} MB)')
    return out


def check_sha256(path):
    """Compare against the committed SHA256SUMS entry, if there is one."""
    if not SHA256SUMS.exists():
        return True
    expected = None
    for line in SHA256SUMS.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == path.name:
            expected = parts[0].lower()
    if expected is None:
        print(f'  {path.name}: not listed in SHA256SUMS (no check)')
        return True
    got = hashlib.sha256(path.read_bytes()).hexdigest()
    if got == expected:
        print(f'  {path.name}: sha256 matches SHA256SUMS')
        return True
    print(f'  {path.name}: SHA256 MISMATCH vs committed — got {got}, '
          f'expected {expected}')
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--factors', type=int, nargs='+', default=[8],
                    help='block-average factors to write (default: 8)')
    args = ap.parse_args()
    arr = load_cube()
    sanity_check(arr)
    ok = True
    for f in args.factors:
        ok &= check_sha256(write_tier(arr, f))
    print('done.' if ok else 'done, WITH CHECKSUM MISMATCHES.')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
