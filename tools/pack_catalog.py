#!/usr/bin/env python3
"""Pack any x,y,z,weight CSV into a Polyphorm dataset (.bin + metadata).

Generic sibling of pack_vac_catalog.py — that script is deliberately
hard-wired to the SDSS VAC (point-count and unit hard-fails, grid
provenance checks); this one packs any catalog in the same CSV layout,
e.g. PolyPhy's 2MRS export:

    python3 tools/pack_catalog.py \
        --csv ~/Development/vendor/python/PolyPhy/data/csv/2mrs_gui.csv \
        --out bin/data/2MRS/2mrs_gui

Then run with `--dataset data/2MRS/2mrs_gui` (from bin/). Output format
matches the upstream packer: float32 (N,4) rows straight to .bin, plus
the positional-key metadata file main.cpp parses.
"""

import argparse
import os
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser(
        description="Pack an x,y,z,weight CSV into a Polyphorm dataset")
    ap.add_argument("--csv", required=True)
    ap.add_argument("--out", required=True,
                    help="output base path, no extension "
                         "(writes <out>.bin + <out>_metadata.txt)")
    ap.add_argument("--weight-scale", type=float, default=1.0,
                    help="multiply weights by this (default 1.0 — Polyphorm "
                         "normalizes by the metadata mean weight, so absolute "
                         "units mostly shift trace magnitudes)")
    args = ap.parse_args()

    data = np.loadtxt(args.csv, delimiter=",")
    if data.ndim != 2 or data.shape[1] != 4:
        sys.exit(f"FATAL: expected 4 columns (x,y,z,weight), got {data.shape}")
    data[:, 3] *= args.weight_scale
    if not (data[:, 3] > 0).all():
        sys.exit("FATAL: non-positive weights")
    data = data.astype(np.float32)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out + "_metadata.txt", "w") as f:
        f.write("Number of points = " + str(data.shape[0]) + "\n")
        f.write("Min X = " + str(np.min(data[:, 0])) + "\n")
        f.write("Max X = " + str(np.max(data[:, 0])) + "\n")
        f.write("Min Y = " + str(np.min(data[:, 1])) + "\n")
        f.write("Max Y = " + str(np.max(data[:, 1])) + "\n")
        f.write("Min Z = " + str(np.min(data[:, 2])) + "\n")
        f.write("Max Z = " + str(np.max(data[:, 2])) + "\n")
        f.write("Mean weight = " + str(np.mean(data[:, 3])) + "\n")
    data.tofile(args.out + ".bin")

    bbox_min = data[:, :3].min(axis=0)
    bbox_max = data[:, :3].max(axis=0)
    print(f"packed {data.shape[0]} points -> {args.out}.bin "
          f"({os.path.getsize(args.out + '.bin')} bytes)")
    print(f"bbox min {bbox_min}")
    print(f"bbox max {bbox_max}")
    print(f"extent {bbox_max - bbox_min}, mean weight {np.mean(data[:, 3])}")


if __name__ == "__main__":
    main()
