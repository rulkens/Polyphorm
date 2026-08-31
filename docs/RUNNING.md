# Running Polyphorm (macOS / WebGPU port)

Quick reference for building and running everything this port can do.
State as of tag `v1.0-macos-port` (2026-08-13): all five port milestones
complete; simulation validated against the published SDSS Cosmic Slime
VAC at 3D log-trace Pearson +0.964 (masked) / +0.957 (unmasked) at d8.

## Build

```sh
cmake -B build                      # configure (incremental — NEVER wipe build/)
nice -n 19 cmake --build build -j 8
```

`build/` holds ~1.4 GB of compiled Dawn; a clean rebuild takes ~1 h.
Always build incrementally.

## Tests

```sh
cd build && ctest --output-on-failure
```

Seven suites (cpplib, file_system, graphics, shader_compile, render_path,
sim_kernel, energy_smoke). All must be green before any commit. The
energy_smoke suite runs a real 400-iteration headless sim on a synthetic
dataset; its first run after a rebuild is slower (cold Metal shader cache).

## Datasets

The binary runs from `bin/` as working directory and reads `config.polyp`
from there. Two SDSS datasets live in `bin/data/SDSS/` (gitignored):

| dataset | points | use |
|---|---|---|
| `galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0` | 37,655 | shipped upstream viz slice, quick runs |
| `sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0` | 324,901 | full VAC catalog (M5 validation input) |

The full catalog is generated from PolyPhy's CSV (read-only repo):

```sh
python3 tools/pack_vac_catalog.py            # defaults do the right thing
python3 tools/pack_vac_catalog.py --verify-grid   # predict the C++ grid fit (712x1200x728)
```

`bin/config.polyp` is set for the validation configuration: 10M agents,
`Grid resolution = 1200`, padding 0.1 → auto-fits the VAC catalog to
exactly 712×1200×728 (0.78 Mpc voxels, ~7.8 GB RSS). Compile-time
simulation constants in `main.cpp` are the SDSS VAC set (sense 4.6,
persistence 0.8, sharpness 2.5); the old SDSS-large set is preserved in
comments next to them.

## GUI

```sh
cd bin
../build/polyphorm --dataset data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0   # quick
../build/polyphorm --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0  # full VAC
```

`--dataset` is required for windowed runs (the compile-time default file
is not shipped). `--agents N` starts with N active agents (clamped to the
`config.polyp` maximum; the AGENT COUNT combo can still change it later).
The launcher scripts `./run_sdss.sh` (`--quick` for the viz slice) and
`./run_2mrs.sh` wrap these invocations and start at 1M agents; extra
arguments are forwarded to the binary. Controls are documented in-app: the **SHORTCUTS** panel
(collapsed by default). Highlights: F1 UI, F2 full reset, F3 pause,
F6 export, F8 clear trace, left-drag orbit / right-drag pan / scroll zoom.

Performance levers while exploring: the **AGENT COUNT** dropdown (1M–10M,
frame cost is roughly linear; switching does a full reset) and the
**TRACE HISTOGRAM** toggle (off = skips a blocking GPU readback per frame).

## Headless + export

```sh
cd bin
../build/polyphorm --headless 1000 --export --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0
```

Runs N iterations without a window, prints the trace-energy series E(i)
every 50 iterations (exit code asserts energy rising), and `--export`
fires the F6 export on the final iteration. Note: stdout is
block-buffered when redirected to a file — an empty log mid-run is
normal; check process aliveness instead.

Exports (F6 in the GUI, or `--export`) land in a timestamped folder
`bin/export/YYYY-MM-DD_HH-MM-SS/` containing:

- `trace.bin`, `deposit.bin` — headerless f16, tightly packed, Z-major /
  X-fastest (`index = z*W*H + y*W + x`), dims in the metadata file.
  ~1.2 GB each at the VAC grid.
- `export_metadata.txt` — dataset, dims, domain, sim parameters.
- `halos_measurements.csv` — per-galaxy trace samples.

The M5 validation run (1000 iterations, ΔE 0.18% over the last 200)
took ~5 min and peaked at 7.8 GB RSS on an M-series/64 GB machine.

## Blender (OpenVDB)

```sh
.venv/bin/python tools/export_vdb.py bin/export/<timestamp>/
```

Writes `volume.vdb` next to the input (or `--output`). Defaults tuned for
Blender: 1/2-resolution block-average (`--downsample 1..4`; 1 = native),
haze cut at raw trace 0.1 (`--threshold`; ~1.0 = filaments only),
log ramp with p99.9 white point (`--white`), 1 Mpc = 1 cm (`--scale`),
grid named `density` so Principled Volume picks it up. `--field both`
adds the deposit as a second grid; `--raw` skips the transfer function.

In Blender: Add > Volume > Import OpenVDB, rendered viewport (Cycles),
and raise Principled Volume density to ~50–500.

The `openvdb` Python module has no macOS wheel — it lives in the
repo-local `.venv/`, built from OpenVDB v13 source against a brew
`openvdb`. If `.venv/` is ever lost, the full rebuild recipe is in the
`tools/export_vdb.py` docstring.

## Validation against the published VAC

```sh
.venv/bin/python tools/validate/compare_trace.py \
    --export-dir bin/export/<timestamp> \
    --reference ~/Development/js/skymap/data/raw/mcpm/mcpm_sdss_d8.npy \
    --out /tmp/report
```

Block-averages the export to the reference's d8 grid (89×150×91),
compares log10(x+1e-3) via Pearson (3D + axis max-projections, masked and
unmasked), renders sanity projections, and prints numbers without a
pass/fail verdict. `--orientation-scan` re-runs the 8-flip axis check
(identity must win). The M5 record and accepted numbers live in
`docs/superpowers/research/m5/m5-run-log.md` and
`docs/superpowers/research/m5/first-measurement/`.

## Known stubs / parked items

- F7 frame capture and `1` HDR screenshot are `warn_once` stubs.
- HUD/histogram overlay geometry is anchored to the startup window size
  (upstream behavior, preserved).
- Performance follow-up (currently ~300 ms/frame at 10M agents native
  grid): profile first — see the perf notes in the project memory and
  `docs/superpowers/research/m4/m4b-carryovers.md`.
