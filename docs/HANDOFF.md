# macOS port: build and run notes

The `macos-webgpu-port` branch (tagged `v1.0-macos-port`) is a native
macOS port of Polyphorm. The renderer was moved from Direct3D 11 to
WebGPU via Google Dawn on Metal, the shaders from HLSL to WGSL, and the
UI to Dear ImGui. The port tries to preserve upstream behavior exactly,
including some of its quirks, instead of fixing things along the way.
The simulation output was validated against the published SDSS Cosmic
Slime VAC and matches at a 3D log-trace Pearson correlation of +0.964
(masked) / +0.957 (unmasked) at d8 after 1000 iterations.

This file gets you from a clean machine to a first run. See
`docs/RUNNING.md` for the rest: headless runs, data export,
Blender/OpenVDB, and the validation pipeline.

## Prerequisites

You need an Apple Silicon Mac. Development happened on an M-series
machine; the heaviest run (10M agents on the 1200 grid) peaks at about
7.8 GB RSS, so 16 GB of RAM is plenty.

- Xcode Command Line Tools: `xcode-select --install`
- CMake 3.24 or newer, plus git: `brew install cmake`
- Python 3 with numpy, only needed for packing datasets
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
cmake --build build -j 8
```

The first configure downloads Dawn and its dependencies, which takes
10-20 minutes. The first build takes up to an hour and leaves `build/`
at about 1.4 GB. Everything after that is incremental, so avoid wiping
`build/`.

To check the result:

```sh
cd build && ctest --output-on-failure
```

All seven suites should pass. `energy_smoke` runs a real 400-iteration
headless simulation on a synthetic dataset and is slower on its first
run after a build because the Metal shader cache is cold.

## Datasets

The repo contains no data. The input format is unchanged from upstream
Polyphorm: a `.bin` of float32 XYZW records and the positional
`_metadata.txt` with point count, extrema and mean weight.

The 37.6k SDSS viz slice you already distribute works as-is; put the
`.bin` and `_metadata.txt` pair at
`bin/data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0`.

Any other x,y,z,weight CSV can be packed with the generic packer:

```sh
python3 tools/pack_catalog.py --csv path/to/catalog.csv --out bin/data/2MRS/2mrs_gui
```

The full SDSS VAC catalog (324,901 galaxies, the validation input) is
packed from PolyPhy's `sample_3D_linW.csv` with
`tools/pack_vac_catalog.py --csv <path>`. That script hard-fails on
point count and unit mismatches.

`bin/config.polyp` is tracked and holds the validation configuration:
10M agents, grid resolution 1200, which auto-fits the VAC catalog to
712x1200x728. The compile-time MCPM constants in `main.cpp` are the
SDSS VAC values (sense 4.6, persistence 0.8, sharpness 2.5); the old
SDSS-large values are still there in comments next to them.

## Running

```sh
./run_sdss.sh --quick   # 37k viz slice
./run_sdss.sh           # full VAC catalog
./run_2mrs.sh           # 2MRS, if packed
```

The scripts start at 1M active agents to keep the frame rate reasonable
(the AGENT COUNT dropdown goes up to 10M, and frame cost is roughly
linear in agent count). Extra arguments are passed through to the
binary. To invoke the binary directly, run it from `bin/`, since it
reads `config.polyp` from the working directory:

```sh
cd bin
../build/polyphorm --dataset data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0 --agents 1000000
```

Keyboard and mouse controls are listed in the SHORTCUTS panel inside
the app (collapsed by default). The important ones: F1 toggles the UI,
F2 does a full reset, F3 pauses, F6 exports, F8 clears the trace;
left-drag orbits, right-drag pans, scroll zooms.

Headless mode, the F6/`--export` output format, the Blender/OpenVDB
converter and the VAC comparison pipeline are documented in
`docs/RUNNING.md`. One caveat there: the OpenVDB Python bindings have
no macOS wheel, so they have to be built from source; the recipe is in
the `tools/export_vdb.py` docstring.

## Known issues

- A frame currently takes about 300 ms at 10M agents on the native VAC
  grid. Profiling and optimization are the next planned work (command
  buffer batching, reshaping the blit workgroup, an f16 trace field).
  Turning off TRACE HISTOGRAM helps right away, since it does a
  blocking GPU readback every frame.
- F7 frame capture and the `1` HDR screenshot are stubs that warn once.
- The HUD and histogram overlays are anchored to the startup window
  size. Upstream does the same; the port keeps it.
