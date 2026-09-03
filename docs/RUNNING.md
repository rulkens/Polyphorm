# Building and running Polyphorm on macOS

The `macos-webgpu-port` branch is a native macOS port of Polyphorm. The
renderer was moved from Direct3D 11 to WebGPU via Google Dawn on Metal,
the shaders from HLSL to WGSL, and the UI to Dear ImGui. The port tries
to preserve upstream behavior exactly, including some of its quirks,
instead of fixing things along the way. The simulation output was
validated against the published SDSS Cosmic Slime VAC and matches at a
3D log-trace Pearson correlation of +0.964 (masked) / +0.957 (unmasked)
at d8 after 1000 iterations. The tag `v1.0-macos-port` marks the point
where that validation closed; the branch has picked up conveniences
since (2MRS support, the `--agents` flag, launcher scripts), all
covered below.

## Prerequisites

You need an Apple Silicon Mac. Development happened on an M-series
machine; the heaviest run (10M agents on the 1200 grid) peaks at about
7.8 GB RSS, so 16 GB of RAM is plenty.

- Xcode Command Line Tools: `xcode-select --install`
- CMake 3.24 or newer, plus git: `brew install cmake`
- Python 3.11 or newer for the offline tools (see Python environment)
- Network access during the first CMake configure

There is no depot_tools setup and no manual Dawn checkout. CMake pulls
Dawn (pinned to release tag `v20260807.193620`) and Dear ImGui (pinned
to v1.92.9) through FetchContent, and Dawn fetches its own
dependencies, GLFW included.

## Build

```sh
git clone -b macos-webgpu-port https://github.com/rulkens/Polyphorm
cd Polyphorm
cmake -B build
nice -n 19 cmake --build build -j 8
```

The first configure downloads Dawn and its dependencies, which takes
10-20 minutes. The first build takes up to an hour and leaves `build/`
at about 1.4 GB. Everything after that is incremental, so avoid wiping
`build/`.

## Tests

```sh
cd build && ctest --output-on-failure
```

Seven suites (cpplib, file_system, graphics, shader_compile,
render_path, sim_kernel, energy_smoke). All must be green before any
commit. `energy_smoke` runs a real 400-iteration headless simulation on
a synthetic dataset and is slower on its first run after a build
because the Metal shader cache is cold.

## Python environment

Every Python tool in `tools/` runs from the repo-local `.venv`, and
every command in this document invokes it as `.venv/bin/python`. Create
it once:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

That covers packing, validation and reference extraction. The Blender
exporter additionally needs the `openvdb` module built from source into
the same venv; the recipe is in the `tools/export_vdb.py` docstring.

## Datasets

The input format is unchanged from upstream Polyphorm: a `.bin` of
float32 XYZW records and the positional `_metadata.txt` with point
count, extrema and mean weight. The binary runs with `bin/` as working
directory and reads `config.polyp` from there. `bin/data/` is tracked;
derived packs and large fetched files are gitignored per subfolder.

Three catalogs are in use on this branch:

- `data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0`, the 37,655
  galaxy viz slice shipped with upstream Polyphorm, tracked in the repo.
- `data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0`,
  the full VAC catalog (324,901 galaxies, the validation input). Not
  tracked; packed in seconds from the vendored
  `bin/data/reference/sample_3D_linW.csv`:

  ```sh
  .venv/bin/python tools/pack_vac_catalog.py                 # defaults do the right thing
  .venv/bin/python tools/pack_vac_catalog.py --verify-grid   # predict the C++ grid fit (712x1200x728)
  ```

  The script hard-fails on point count and unit mismatches.
- `data/2MRS/2mrs_gui`, the 2MRS catalog (34,974 galaxies, equatorial
  cartesian Mpc), tracked in the repo as both the source CSV and the
  packed pair; see `bin/data/2MRS/README.md`. It is an interactive
  extra, not part of the validation. Any x,y,z,weight CSV in this
  layout can be packed with the generic packer:

  ```sh
  .venv/bin/python tools/pack_catalog.py --csv bin/data/2MRS/2mrs_gui.csv --out bin/data/2MRS/2mrs_gui
  ```

`bin/data/reference/` holds the validation inputs that did not originate
here, byte-identical to their public sources: the VAC input catalog,
the VAC's own `export_metadata.txt`, and the d8 reference cube. Its
`README.md` records provenance, checksums and lineage.

`bin/config.polyp` is tracked and holds the validation configuration:
10M agents, grid resolution 1200, padding 0.1, which auto-fits the VAC
catalog to exactly 712x1200x728 (0.78 Mpc voxels, about 7.8 GB RSS).
The compile-time MCPM constants in `main.cpp` are the SDSS VAC values
(sense 4.6, persistence 0.8, sharpness 2.5); the old SDSS-large values
are still there in comments next to them.

## Running the GUI

```sh
./run_sdss.sh --quick   # 37k viz slice
./run_sdss.sh           # full VAC catalog
./run_2mrs.sh           # 2MRS
```

The scripts start at 1M active agents to keep the frame rate reasonable
and pass extra arguments through to the binary. To invoke the binary
directly, run it from `bin/`:

```sh
cd bin
../build/polyphorm --dataset data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0 --agents 1000000
```

`--dataset` is required for windowed runs, since the compile-time
default file is not shipped. `--agents N` sets the starting active
agent count, clamped to the `config.polyp` maximum; the AGENT COUNT
dropdown can still change it later.

Keyboard and mouse controls are listed in the SHORTCUTS panel inside
the app (collapsed by default). The important ones: F1 toggles the UI,
F2 does a full reset, F3 pauses, F6 exports, F8 clears the trace;
left-drag orbits, right-drag pans, scroll zooms.

Two things help interactive frame rates: the AGENT COUNT dropdown
(1M-10M, frame cost is roughly linear, switching does a full reset) and
turning TRACE HISTOGRAM off, which skips a blocking GPU readback every
frame.

## Headless runs and export

```sh
cd bin
../build/polyphorm --headless 1000 --export --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0
```

Runs N iterations without a window and prints the trace energy E every
50 iterations; the exit code asserts that energy rose over the run.
`--export` fires the F6 export on the final iteration. Note that stdout
is block-buffered when redirected to a file, so an empty log mid-run is
normal; check process aliveness instead.

Exports (F6 in the GUI, or `--export`) land in a timestamped folder
`bin/export/YYYY-MM-DD_HH-MM-SS/` containing:

- `trace.bin` and `deposit.bin`: headerless f16, tightly packed,
  Z-major / X-fastest (`index = z*W*H + y*W + x`), dims in the metadata
  file. About 1.2 GB each at the VAC grid.
- `export_metadata.txt`: dataset, dims, domain, sim parameters.
- `halos_measurements.csv`: per-galaxy trace samples.

The M5 validation run (1000 iterations, delta-E 0.18% over the last
200) took about 5 minutes and peaked at 7.8 GB RSS.

## Blender (OpenVDB)

```sh
.venv/bin/python tools/export_vdb.py bin/export/<timestamp>/
```

Writes `volume.vdb` next to the input (or `--output`). The defaults are
tuned for Blender: half-resolution block average (`--downsample 1..4`,
1 is native), haze cut at raw trace 0.1 (`--threshold`; around 1.0
keeps filaments only), log ramp with a p99.9 white point (`--white`),
1 Mpc = 1 cm (`--scale`), and the grid is named `density` so Principled
Volume picks it up. `--field both` adds the deposit as a second grid;
`--raw` skips the transfer function.

In Blender: Add > Volume > Import OpenVDB, rendered viewport (Cycles),
and raise Principled Volume density to somewhere in 50-500.

The `openvdb` Python module has no macOS wheel, so it was built from
OpenVDB v13 source against a brew `openvdb` into the repo-local
`.venv/`. If `.venv/` is ever lost, the full rebuild recipe is in the
`tools/export_vdb.py` docstring.

## Validation against the published VAC

The full chain from a clean checkout, after the build and the Python
environment above:

```sh
.venv/bin/python tools/pack_vac_catalog.py                    # 1. pack the catalog
(cd bin && ../build/polyphorm --headless 1000 --export \
    --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0)  # 2. ~5 min
.venv/bin/python tools/validate/compare_trace.py \
    --export-dir bin/export/<timestamp> --out reports/<name>  # 3. compare
```

Step 2 must use the binary directly with the tracked `config.polyp`
(10M agents, grid 1200, padding 0.1). `run_sdss.sh` is a GUI
convenience that starts at 1M agents, which also applies headless, so
it is not a substitute here unless `--agents 10000000` is passed.

Step 3 block-averages the export to the reference's d8 grid
(89x150x91), compares log10(x+1e-3) via Pearson (3D plus axis
max-projections, masked and unmasked), renders sanity projections, and
prints numbers without a pass/fail verdict. The reference defaults to
the vendored `bin/data/reference/mcpm_sdss_d8.npy`. `--orientation-scan`
re-runs the 8-flip axis check (identity must win);
`--self-test` checks the tool against synthetic cubes and the
reference itself.

To re-derive the reference cube from the SDSS archive instead of
trusting the vendored copy:

```sh
.venv/bin/python tools/validate/download_vac.py        # 345 MB from data.sdss.org, sha1-verified
.venv/bin/python tools/validate/extract_reference.py   # ~1 min, 10 GB RAM, checks SHA256SUMS
```

The M5 record and accepted numbers live in
`docs/superpowers/research/m5/m5-run-log.md` and
`docs/superpowers/research/m5/first-measurement/`.

## Known issues

- A frame currently takes about 300 ms at 10M agents on the native VAC
  grid. Profiling and optimization are the next planned work (command
  buffer batching, reshaping the blit workgroup, an f16 trace field);
  see `docs/superpowers/research/m4/m4b-carryovers.md`.
- F7 frame capture and the `1` HDR screenshot are stubs that warn once.
- The HUD and histogram overlays are anchored to the startup window
  size. Upstream does the same; the port keeps it.
