# Polyphorm macOS port — C++ + Dawn/WebGPU, complete fork

Polyphorm is Windows-only: D3D11 for both the MCPM simulation (compute) and
volume rendering, Win32 platform layer, MSVC-only build tool, and Windows-only
deps (XAudio2, a prebuilt freetype `.lib`, DirectXTex). This fork ports it to
run natively on macOS by replacing the GPU/platform layers with WebGPU (Dawn)
+ GLFW and the custom UI with Dear ImGui, while keeping `main.cpp`'s
scientific core — the simulation flow, parameter system, and analysis loop —
intact and *bug-for-bug faithful* until validated.

This is a **complete fork**: it will never merge upstream (upstream is
dormant since 2021). Dead subsystems are deleted, not preserved.

## Goal

- The core analysis loop runs interactively on macOS: MCPM fitting on the
  bundled SDSS dataset, particle mode, trace volume mode, live parameter
  sliders, energy plot + density histogram.
- **Success bar = validated reconstruction**: the exported trace cube,
  fitted on the SDSS DR17 VAC input catalog with the published VAC
  parameters, correlates strongly with the published Cosmic Slime cube
  (rhizome `compareCubes.py` at the d4/d8 downsampled tiers; voxelwise
  Pearson on log-trace, histogram shape, energy convergence). Visual
  parity alone does not close the milestone.
- F6 export keeps the existing on-disk contract (raw f16 `.bin` +
  `export_metadata.txt`) so `OpenPolyphorm.ipynb`, pyslime, and skymap's
  `extractMcpmCube.py` work unmodified.

## Non-goals (deferred, not dropped)

- Overdensity / highlights / velocity / halo-color modes and the
  volumetric path tracer (`cs_volpath`): after validation.
- `VELOCITY_ANALYSIS` / `HALO_COLOR_ANALYSIS` compile paths, agent sort,
  proxy objects, screen capture (F7/'1'), F5 agent capture: after
  validation.
- f16 grid storage (memory optimization via packed u32 buffers): only if
  r32float memory becomes a real constraint.
- Windows/D3D11 support: gone. WebGPU itself is cross-platform; if the
  fork ever needs Windows it goes through Dawn, not D3D11.

## Scope of the first milestone chain

M1 window + Dawn device + cleared swapchain → M2 simulation dispatches
running, energy rising (headless log sufficient) → M3 particle mode
renders → M4 trace volume mode + ImGui parameter panel + plots →
M5 export + VAC validation. Each milestone is runnable and demo-able.

## Stack

- **C++17, CMake.** `builder/`, `build_and_run.bat`, `polyphorm.build`
  deleted. Single `polyphorm` target + a `shaders/` install step (WGSL is
  loaded at runtime like the HLSL was, preserving shader-edit iteration).
- **Dawn** (Google's WebGPU), fetched via CMake — chosen over wgpu-native
  because it is Chrome's engine: identical Tint WGSL semantics to the
  browser WebGPU work in skymap, and best-in-class validation messages
  during shader translation. `webgpu_cpp.h` API.
- **GLFW** for window/input; **Dear ImGui + ImPlot** (vendored) for UI;
  **stb_image** (vendored, single header) replaces DirectXTex's
  `LoadFromTGAFile` inside `graphics::load_texture2D` — the palette TGAs
  are its only load-bearing use.
- Deleted: `ovr.*`, `oculus/`, `audio.*`, `stb_vorbis.c`, `fbx_loader.*`,
  `ui.*`, `font.*`, `cpplib/freetype/`, `cpplib/fonts/`, `resources.*`
  (D3D-tied), DirectXTex dependency.
- Kept as-is: `maths`, `memory`, `random`, `logging`, `parsers`, `stack.h`,
  `array.h`.
- Ported preserving their public APIs (so `main.cpp` call sites survive):
  `platform.*` + `input.*` (Win32 → GLFW), `file_system.*`,
  `graphics.*` (D3D11 → WebGPU; same function shapes, new internals).
- `main.cpp`: simulation flow, frame loop, F-key handling, parameter
  conversions untouched; `ui::` call sites become ImGui/ImPlot
  (`ui::add_slider` → `ImGui::SliderFloat`, same returns-changed
  contract that drives `reset_pt`); energy plot + histogram → ImPlot.

## GPU resource mapping

| D3D11 (original) | WebGPU (port) |
|---|---|
| `cbuffer b0` (SimulationConfig / StatisticsConfig), `b4` (RenderingConfig) | uniform buffers; one bind group per pass |
| six `RWStructuredBuffer<float>` u2–u7 (particle SoA) | storage buffers, read_write |
| deposit A/B, trace `RWTexture3D<half…>` R16F | **`r32float` storage textures** (WebGPU has no r16float storage; r32float is the only float format with read_write access — required by the in-place racy accumulation) |
| non-atomic UAV `+=` (deliberate: no float atomics on GPUs; races launder into MCPM's Monte Carlo noise) | same racy textureLoad/textureStore on r32float read_write — a faithful port of the same considered trade, since WGSL also has no float atomics |
| trilinear+aniso sampling of trace in volume shaders | sampled float texture + linear sampler; requires the **`float32-filterable`** device feature (available on Apple Silicon Metal) |
| `InterlockedAdd`/`InterlockedMax` (histogram, particle splat) | `atomicAdd`/`atomicMax` on u32 storage buffers |
| `R32_UINT` splat image + `R32G32B32A32_FLOAT` display tex | same formats (both core WebGPU) |
| `Present(1,0)` vsync | FIFO present mode |

Memory at f32: 1024-class grid ≈ 2× the original's f16 (~4 GB textures);
the VAC-scale 712×1200×728 validation grid ≈ 7.5 GB. Both fit the target
machine (Apple Silicon unified memory); `Grid resolution` stays a
`config.polyp` knob for smaller machines. Readback for export/statistics
uses staging buffers (D3D11's direct-Map-of-default-heap trick does not
exist in WebGPU and was nonstandard anyway).

## Shader ports — 9 for core, bug-for-bug

| HLSL | WGSL notes |
|---|---|
| `cs_agents_propagate` | the algorithm core; f16 texture types → f32; `int3()` truncation → `vec3<i32>()` (same toward-zero semantics); identical `wang_hash`/xorshift RNG constants |
| `cs_field_decay` | **keep** the `all(int3(...))` weighting quirk (19 taps at 1.0 + 8 corners at 0.577, not the intended per-shell falloff) and the non-periodic low-side `%` boundary (HLSL and WGSL `%` share sign-of-dividend semantics, so the port is literal); keep the dithered trace decay |
| `cs_density_histo` | atomics on u32 buffer; `sample_randomly` null-model toggle kept |
| `cs_particles_transform` | atomic u32 splat (+10 agents / +10000 data); trim box kept |
| `cs_particles_blit` | **only sanctioned deviation**: `numthreads(1,1,1)` → 8×8 workgroup; provably identical output, removes a 1.8M-dispatch absurdity |
| `vs_2d`, `vs_3d` | texcoord permutation switch (`texcoord_map ∈ {±1,±2,±3}`) ported literally |
| `ps_particles_color` | data/agent color split at the 10000 threshold, kept |
| `ps_volume_trace` | slice-stack compositing kept (1024 view-aligned quads, over-blend, `rgb *= 2.0` one-stack compensation) — **not** rewritten as a raymarcher; palette TGAs unchanged |

Every preserved quirk gets a `// QUIRK(<name>): kept for VAC parity` comment
and a post-validation cleanup ticket. The dispatch truncation
(`Dispatch(10,10,N/100000)` skipping the last `N % 100000` particles —
data points are at the buffer front and always run) is likewise kept until
validation passes.

Consequence of the racy accumulation carried over: runs are statistically,
never bitwise, reproducible — validation is correlation-based by design.

## Data pipeline

Input formats (`.bin` XYZW float32 + positional `_metadata.txt`),
`config.polyp`, and the compile-time `REGIME_*` system are unchanged in
M1–M5. The VAC input catalog for validation is packed from PolyPhy's
`sample_3D_linW.csv` (= the published 324,849-pt input within float32
noise; weights ×10⁻³ to restore the 10¹² M☉ unit convention Polyphorm's
`log10(1+W)` load path expects — see rhizome `DATA_LINEAGE.md`).

F6 export: GPU readback → f32→f16 CPU conversion → raw `.bin` (Z-major,
same layout DirectXTex produced) + `export_metadata.txt` with identical
key set. `.dds` output dropped. `halos_measurements.csv` and F9/F10
visu-state save/load kept (plain file I/O, no platform coupling).

## Validation (M5, the gate)

1. Pack VAC input; run with `sdss_vac_metadata.txt` parameters (sense
   4.6 Mpc, move 0.1 Mpc, spread 20°/10°, persistence 0.8, sharpness 2.5,
   agent deposit 0, 10M agents, grid 712×1200×728) to convergence
   (~1000+ iterations, energy plateau).
2. F6 export → `tools/validate/` Python script (lives in this fork)
   downsamples ×4/×8 by block-mean and compares against skymap's cached
   `mcpm_sdss_d{4,8}.npy` tiers using the rhizome `compareCubes.py`
   metrics: voxelwise Pearson on `log10(trace + ε)` over the joint
   support, marginal histograms, and the equilibrium energy value.
3. Pass bar: log-trace Pearson ≥ 0.9 at d8 (the PolyPhy fork's best
   attempt scored ≈ 0.0, so the metric separates real success from
   plausible-looking haze), histogram shape qualitatively matching the
   published bell curve. Misses trigger quirk-by-quirk A/B hunts (each
   preserved quirk is independently toggleable via a `#define`).
4. RSD caveat recorded: the published cube used the `rsdCorr` DBSCAN
   preprocessing; the CSV already contains those corrected positions, so
   no preprocessing is reimplemented here.

## Error handling

Dawn error callbacks (validation, device-lost) route through `logging`
with shader/pass labels on every pipeline and pass. Failure to get the
`float32-filterable` feature or a big-enough `maxBufferSize`/texture
limit at startup is a clear fatal message naming the limit, not a later
mystery crash. `config.polyp` parse failures keep the existing
positional-parser behavior (fork may harden later; out of scope).

## Testing

- The primary test is the M5 validation gate (statistical, scripted,
  repeatable).
- CPU-side unit tests (small CTest target): metadata/`config.polyp`
  parsing, world↔grid conversions (including the cubic-voxel rescale),
  f32→f16 export conversion, VAC CSV→bin packer round-trip.
- Per-milestone smoke: M2 asserts energy strictly rises over the first
  N iterations on the bundled SDSS sample and the histogram's null bin
  shrinks — catching sign/coordinate errors long before M5.
- GPU shader correctness is exercised through the milestones themselves;
  no shader unit-test harness in this scope.

## Risks

- **Dawn build weight**: first configure is heavy. Accepted; pinned Dawn
  revision recorded in CMake for reproducibility.
- **7.5 GB validation grid**: fine on the primary machine; the
  `Grid resolution` knob plus d8-only comparison is the fallback.
- **Quirk interactions**: if M5 misses the bar, the per-quirk `#define`
  toggles turn debugging into bisection rather than archaeology.
- **Slice-stack rendering artifacts** under WebGPU blending: cosmetic
  only; validation reads the exported cube, not pixels.
