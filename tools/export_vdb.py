#!/usr/bin/env python3
"""Convert a Polyphorm F6/--export dump to OpenVDB for Blender.

Reads the headerless f16 .bin volumes (trace.bin / deposit.bin) plus
export_metadata.txt from an export folder and writes a .vdb file that
Blender imports directly (Add > Volume > Import OpenVDB, or drag & drop).

The trace grid is named "density" so Blender's default Principled Volume
shader picks it up without renaming. By default values are log-compressed
and normalized to [0,1] (the raw trace spans many decades, which renders
as a near-empty volume); pass --raw to export the untouched values.

Requires the OpenVDB Python bindings (module `openvdb`), which have no
macOS PyPI wheel. One-time setup used for this repo (venv at .venv/):

    brew install openvdb                     # libopenvdb + deps
    python3 -m venv .venv
    .venv/bin/pip install numpy nanobind
    git clone --depth 1 --branch v13.0.0 \
        https://github.com/AcademySoftwareFoundation/openvdb /tmp/openvdb
    cmake -S /tmp/openvdb -B /tmp/openvdb-py \
        -DOPENVDB_BUILD_CORE=OFF -DOPENVDB_BUILD_BINARIES=OFF \
        -DOPENVDB_BUILD_PYTHON_MODULE=ON -DUSE_NUMPY=ON \
        -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
        -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
        -Dnanobind_DIR="$(.venv/bin/python -m nanobind --cmake_dir)"
    cmake --build /tmp/openvdb-py -j 8
    # then copy the built openvdb module into .venv site-packages

Usage:
    .venv/bin/python tools/export_vdb.py bin/export/<timestamp>/
    .venv/bin/python tools/export_vdb.py bin/export/<timestamp>/ \
        --field both --output cosmic_web.vdb
"""

import argparse
import re
import sys
from pathlib import Path

import numpy as np


def parse_metadata(path):
    """Return ((res_x, res_y, res_z), (size_x, size_y, size_z) in Mpc)."""
    text = path.read_text()
    m_res = re.search(
        r"simulation grid resolution:\s*(\d+)\s*x\s*(\d+)\s*x\s*(\d+)", text)
    m_size = re.search(
        r"simulation grid size:\s*([\d.]+)\s*x\s*([\d.]+)\s*x\s*([\d.]+)", text)
    if not m_res or not m_size:
        sys.exit(f"error: cannot parse grid resolution/size from {path}")
    return (tuple(int(g) for g in m_res.groups()),
            tuple(float(g) for g in m_size.groups()))


def load_volume(path, res):
    """Load a headerless f16 export as a float32 array indexed [x, y, z].

    File layout (export byte-parity contract, see compare_trace.py):
    tightly packed f16, index = z*W*H + y*W + x (Z-major, X-fastest).
    """
    rx, ry, rz = res
    expected = rx * ry * rz * 2
    actual = path.stat().st_size
    if actual != expected:
        sys.exit(f"error: {path} is {actual} bytes, expected {expected} "
                 f"for grid {rx}x{ry}x{rz} (f16)")
    data = np.fromfile(path, dtype=np.float16).astype(np.float32)
    data = data.reshape(rz, ry, rx)            # [z, y, x]
    return np.ascontiguousarray(data.transpose(2, 1, 0))   # [x, y, z]


def log_normalize(arr, eps):
    """log10(1 + x/eps), scaled to [0, 1]. Zeros stay exactly zero."""
    np.divide(arr, eps, out=arr)
    np.log1p(arr, out=arr)          # ln(1 + x/eps); constant factor drops
    peak = float(arr.max())
    if peak > 0.0:
        arr /= peak
    return arr


def make_grid(vdb, arr, name, voxel_size, tolerance):
    grid = vdb.FloatGrid()
    grid.copyFromArray(arr, tolerance=tolerance)
    grid.transform = vdb.createLinearTransform(voxelSize=voxel_size)
    grid.name = name
    grid.gridClass = vdb.GridClass.FOG_VOLUME
    grid.saveFloatAsHalf = True
    active = grid.activeVoxelCount()
    print(f"  grid '{name}': {active:,} active voxels "
          f"({100.0 * active / arr.size:.1f}% of {arr.size:,})")
    return grid


def main():
    ap = argparse.ArgumentParser(
        description="Convert a Polyphorm export folder to a Blender-ready .vdb")
    ap.add_argument("export_dir", type=Path,
                    help="export folder containing trace.bin / deposit.bin "
                         "and export_metadata.txt")
    ap.add_argument("--field", choices=("trace", "deposit", "both"),
                    default="trace")
    ap.add_argument("--output", type=Path, default=None,
                    help="output .vdb path (default: <export_dir>/volume.vdb)")
    ap.add_argument("--raw", action="store_true",
                    help="export raw values instead of log-normalized [0,1]")
    ap.add_argument("--eps", type=float, default=1e-2,
                    help="log compression knee: density = log(1 + x/eps), "
                         "normalized (default 1e-2)")
    ap.add_argument("--tolerance", type=float, default=0.0,
                    help="values within this of 0 become inactive background "
                         "(default 0.0: prune exact zeros only)")
    args = ap.parse_args()

    try:
        import openvdb as vdb
    except ImportError:
        sys.exit("error: OpenVDB Python bindings not found — run with the "
                 "repo venv (.venv/bin/python) or see this script's "
                 "docstring for the one-time build recipe.")

    meta = args.export_dir / "export_metadata.txt"
    if not meta.is_file():
        sys.exit(f"error: {meta} not found — pass a Polyphorm export folder")
    res, size_mpc = parse_metadata(meta)
    voxel_sizes = [s / r for s, r in zip(size_mpc, res)]
    voxel_size = sum(voxel_sizes) / 3.0
    spread = (max(voxel_sizes) - min(voxel_sizes)) / voxel_size
    if spread > 0.01:
        print(f"warning: voxels are {spread * 100.0:.1f}% anisotropic "
              f"({voxel_sizes}); using isotropic mean {voxel_size:.5f} Mpc")
    print(f"grid {res[0]}x{res[1]}x{res[2]}, voxel {voxel_size:.5f} Mpc")

    fields = ("trace", "deposit") if args.field == "both" else (args.field,)
    grids = []
    for field in fields:
        print(f"loading {field}.bin ...")
        arr = load_volume(args.export_dir / f"{field}.bin", res)
        if not args.raw:
            arr = log_normalize(arr, args.eps)
        # Blender's Principled Volume reads a grid named "density" by
        # default — the trace (the MCPM result) gets that name.
        name = "density" if field == "trace" else field
        grids.append(make_grid(vdb, arr, name, voxel_size, args.tolerance))
        del arr

    out = args.output or (args.export_dir / "volume.vdb")
    vdb.write(str(out), grids=grids)
    print(f"wrote {out} ({out.stat().st_size / 1e6:.1f} MB)")
    print("Blender: Add > Volume > Import OpenVDB (units are Mpc; scale "
          "down if the scene clips)")


if __name__ == "__main__":
    main()
