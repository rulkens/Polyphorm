# Polyphorm macOS port — handoff notes

This branch (`macos-webgpu-port`, tag `v1.0-macos-port`) is a native
macOS port of Polyphorm: Direct3D 11 → WebGPU (Google Dawn, Metal
backend), HLSL → WGSL, the custom UI → Dear ImGui. The port is
quirk-preserving — simulation behavior was kept bit-faithful where
possible rather than "improved" — and the result was validated against
the published SDSS Cosmic Slime VAC: 3D log-trace Pearson **+0.964**
(masked) / **+0.957** (unmasked) at d8 after 1000 iterations.

`docs/RUNNING.md` is the day-to-day reference (build, tests, GUI,
headless export, Blender/OpenVDB, validation). This file is the
fresh-machine path to a first successful run.

## Prerequisites

- An Apple Silicon Mac. Tested on an M-series machine; the full VAC run
  (10M agents, 1200 grid) peaks at ~7.8 GB RSS, so 16 GB RAM is enough.
- Xcode Command Line Tools (`xcode-select --install`).
- CMake ≥ 3.24 and git (`brew install cmake`).
- Python 3 with numpy — only needed to pack datasets (see below).
- Network access on the first CMake configure (dependency fetch).

No depot_tools, no manual Dawn checkout: CMake FetchContent pulls Dawn
(pinned to release tag `v20260807.193620`) and Dear ImGui (pinned
commit, v1.92.9) and Dawn fetches its own dependencies, GLFW included.

## Build

```sh
git clone -b macos-webgpu-port https://github.com/rulkens/Polyphorm
cd Polyphorm
cmake -B build
cmake --build build -j 8
```

The first configure downloads Dawn and its dependencies (~10–20 min);
the first build takes up to ~1 h and `build/` grows to ~1.4 GB. After
that, builds are incremental and fast — don't wipe `build/`.

Sanity check:

```sh
cd build && ctest --output-on-failure
```

Seven suites; all should be green. `energy_smoke` runs a real
400-iteration headless simulation on a synthetic dataset, so its first
post-build run is slower (cold Metal shader cache).

## Datasets

No data ships in the repo. The input format is unchanged from upstream
Polyphorm: a `.bin` of float32 XYZW records plus the positional
`_metadata.txt` (point count, extrema, mean weight).

- **Existing upstream data drops in directly.** Place the 37.6k SDSS
  viz slice you already distribute at
  `bin/data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0.bin`
  (+ its `_metadata.txt`).
- **Any x,y,z,weight CSV** can be packed with the generic packer:

  ```sh
  python3 tools/pack_catalog.py --csv path/to/catalog.csv --out bin/data/2MRS/2mrs_gui
  ```

- **The full SDSS VAC catalog** (324,901 galaxies, the validation
  input) is packed from PolyPhy's `sample_3D_linW.csv` with
  `tools/pack_vac_catalog.py --csv <path>` — it hard-fails on point
  count and unit mismatches, so a wrong input is unmistakable.

`bin/config.polyp` is tracked and set to the validation configuration
(10M agents, grid resolution 1200 → auto-fits the VAC to 712×1200×728).
Compile-time MCPM constants in `main.cpp` are the SDSS VAC set (sense
4.6, persistence 0.8, sharpness 2.5); the old SDSS-large set is
preserved in comments beside them.

## Run

```sh
./run_sdss.sh --quick   # 37k viz slice
./run_sdss.sh           # full VAC catalog
./run_2mrs.sh           # 2MRS, if packed
```

The scripts start at 1M active agents for interactivity (the AGENT
COUNT dropdown scales up to 10M; frame cost is roughly linear) and
forward extra arguments to the binary. Direct invocation — the binary
must run with `bin/` as working directory:

```sh
cd bin
../build/polyphorm --dataset data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0 --agents 1000000
```

Controls are documented in-app in the **SHORTCUTS** panel (collapsed by
default): F1 UI, F2 full reset, F3 pause, F6 export, F8 clear trace,
left-drag orbit / right-drag pan / scroll zoom.

Headless runs, the F6/`--export` data export, the Blender/OpenVDB
converter, and the VAC validation pipeline are all covered in
`docs/RUNNING.md`. Note the OpenVDB Python bindings have no macOS
wheel; the build-from-source recipe is in the `tools/export_vdb.py`
docstring.

## Known gaps

- Performance: ~300 ms/frame at 10M agents on the native VAC grid.
  Profiling and optimization (command-buffer batching, blit workgroup
  reshape, f16 trace field) are the next planned work; turning off
  TRACE HISTOGRAM helps immediately (it does a blocking readback per
  frame).
- F7 frame capture and the `1` HDR screenshot are warn-once stubs.
- HUD/histogram overlay geometry anchors to the startup window size
  (upstream behavior, preserved).
