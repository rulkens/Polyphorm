# M5 export-path research

Scope: read-only research for the F6 (deposit/trace) and F5 (agents) export
paths, the readback machinery already in the port, and grid-geometry/format
quirks an M5 exporter design needs to account for. Branch
`macos-webgpu-port`; upstream cited via `git show master:<path>`.

## 1. F6 deposit/trace export (M5-critical path)

- F6 sets a flag: `main.cpp:1017` (`if (input::key_pressed(KeyCode::F6)) store_deposit = true;`).
- Export block: `main.cpp:1177-1226`, gated `if (store_deposit)`.
  - `main.cpp:1181-1184`: saves whichever trail texture is "current" —
    `trail_tex_A` if `is_a`, else `trail_tex_B` — to `"export/deposit"`
    via `graphics::save_texture3D`.
  - `main.cpp:1185`: saves `trace_tex` to `"export/trace"`.
  - `main.cpp:1187-1205`: writes `export/export_metadata.txt` (plain
    `std::ofstream` text: dataset name, data-point count, agent count,
    grid resolution X/Y/Z, world size/center in Mpc, move/sense distance
    in Mpc and voxels, angles, decay/deposit/sharpness coefficients).
    Pure CPU string formatting, no GPU dependency.
  - `main.cpp:1207-1223`: `graphics::capture_structured_buffer(&halos_densities_buffer, ...)`
    (already-working GPU→CPU readback, §3) then writes
    `export/halos_measurements.csv` (per-galaxy M200b, trace value, world
    and grid XYZ). **Already works end-to-end** — depends only on
    `capture_structured_buffer`, not `save_texture3D`.
  - This block is a byte-for-byte copy of upstream's (`git show
    master:main.cpp` lines 1071-1119, identical code, only line numbers
    shift) — the port did not change F6's intent, only left the
    texture-save primitive stubbed.

**The stub.** `graphics::save_texture3D` in the port is a no-op:
```
cpplib/graphics.cpp:484-486
void save_texture3D(Texture3D *texture, std::string filename) {
    (void)texture; (void)filename; warn_once("save_texture3D");
}
```
Declared `cpplib/graphics.h:165` (`// M2a: stub (M5)`). `warn_once`
(`cpplib/graphics.cpp:291-303`) prints once per distinct stub name to
stderr, then no-ops. Signature matches upstream: `void save_texture3D
(Texture3D *texture, std::string filename)` — filename has no extension,
each backend appends its own.

**Upstream's real implementation** (`git show master:cpplib/graphics.cpp`
lines 607-631), D3D11 via the external **DirectXTex** library
(`#include <DirectXTex.h>`, `git show master:cpplib/graphics.h:4`; not
vendored, Windows-only external dep):
```
DirectX::ScratchImage image;
DirectX::CaptureTexture(device, context, texture->texture, image);  // staging tex + Map, internal to DirectXTex
... SaveToDDSFile(...) -> filename + ".dds"
size_t tex_byte_size = image.GetPixelsSize();
uint8_t* raw_data = image.GetPixels();
bin_file.write((char*)raw_data, tex_byte_size);   // filename + ".bin"
```
`DirectX::CaptureTexture` (Microsoft DirectXTex; not present in-repo, no
line cite for internals — described from its documented public
behavior) makes a CPU-readable staging texture, `Map()`s it, and
**repacks the GPU-native (possibly padded) row/depth pitch into a
tightly packed `ScratchImage`**: each depth slice is row-major with
`rowPitch == width * bytes_per_texel` (no padding), slices contiguous
(`slicePitch = rowPitch * height`) — so `GetPixels()`/`GetPixelsSize()`
is one contiguous, unpadded buffer. Two files per call: `<filename>.dds`
(unused downstream) and `<filename>.bin` (what `OpenPolyphorm.ipynb`,
pyslime, and skymap's `extractMcpmCube.py` actually read).

**Output byte layout (upstream `.bin`):** tightly packed, X fastest
(within a row), Y next (rows in a slice), Z slowest (slice index) —
`index = z*(width*height) + y*width + x` — "Z-major" per the design doc
(`docs/superpowers/specs/2026-08-10-macos-webgpu-port-design.md:125-127`).
**Bytes/voxel = 2** in upstream's default build: `trail_tex_A/B` and
`trace_tex` are `DXGI_FORMAT_R16_FLOAT` (`git show
master:main.cpp:568-569,574`, the non-`HALO_COLOR_ANALYSIS`/non-
`VELOCITY_ANALYSIS` branch matching the port's active config) — half
precision. (The `HALO_COLOR_ANALYSIS`/`VELOCITY_ANALYSIS` branches use
wider R16G16/R16G16B16A16 formats; out of scope, and the port's
`main.cpp:532,541` `#ifdef` guards are undefined by default.) No header
in the `.bin`; dims/world-size live only in `export_metadata.txt`.

The design doc already commits to the target contract: "GPU readback →
f32→f16 CPU conversion → raw `.bin` (Z-major, same layout DirectXTex
produced) + `export_metadata.txt` with identical key set ... `.dds`
output dropped" (design doc lines 125-127). So M5's job: readback the
r32float 3D texture (port's storage format, §3/§6), convert f32→f16 on
CPU, write tightly-packed Z-major bytes; `export_metadata.txt` is
unchanged (already works, `main.cpp:1187-1205`).

## 2. F5 agents export

Trigger: `main.cpp:1016` (`if (input::key_pressed(KeyCode::F5))
capture_agents = !capture_agents;` — a toggle, not one-shot like F6).
Body: `main.cpp:1229-1264`. Each frame while active (up to
`N_AGENT_TIMESTEPS_TO_CAPTURE` = 10, `main.cpp:196`), reads back
`particles_buffer_x/y/z/weights` via `graphics::capture_structured_buffer`
(`main.cpp:1246-1249` — same already-working primitive as F6's
halo-density capture) and appends a `"*** timestep N [X Y Z D] ***"`
block to `export/agents.txt` (world-space coords via
`measure_grid_to_world`, weight as-is).

Byte-identical to upstream (`git show master:main.cpp:1122-1157`). The
design doc explicitly defers it: "`VELOCITY_ANALYSIS` /
`HALO_COLOR_ANALYSIS` compile paths, agent sort, proxy objects, screen
capture (F7/'1'), F5 agent capture: after validation" (design doc
line 34). **F5 needs no M5 work** — it already works (never touches
`save_texture3D`) and isn't part of the validation gate; file-parity
only, note and move on.

## 3. Existing readback machinery to reuse

**`graphics::capture_structured_buffer`** — `cpplib/graphics.cpp:877-907`,
declared `cpplib/graphics.h:212`. Signature: `(StructuredBuffer *buffer,
void *mapped_data, uint32_t num_elements, size_t element_size)`. Lazily
creates a `MapRead|CopyDst` readback buffer sized to `buffer->size`
(`graphics.cpp:881-886`, cached as `StructuredBuffer::readback`,
`cpplib/graphics.h:88`); records `CopyBufferToBuffer`, flushes the
encoder (`graphics.cpp:890-892`); `MapAsync` then **blocks** by pumping
`ProcessEvents` in a `wait_for` loop (`graphics.cpp:894-903` — same
idiom as `tests/render_path_tests.cpp:112-114`); `memcpy`s from
`GetConstMappedRange`, `Unmap()`s (`graphics.cpp:904-906`). Synchronous
same-frame stall by design (comment `graphics.cpp:887-889`: reproduces
D3D11's `Map(D3D11_MAP_READ)` stall, "latency is a conscious non-goal").
Used today for buffers only (`halos_densities_buffer`,
`particles_buffer_*`, `density_histogram_buffer`) — the copy→flush→
MapAsync→block→memcpy→Unmap pattern is exactly what texture readback
needs too, modulo row-padding.

**No existing texture3D→CPU readback in `cpplib/`** — the actual gap
`save_texture3D` must fill. But the pattern exists for 2D textures in
tests:
- `tests/render_path_tests.cpp:123-164`,
  `readback_rgba32f(wgpu::Texture, width, height, float *out)`: computes
  `padded_bytes_per_row = (width*4*sizeof(float) + 255) & ~255` (Dawn's
  256-byte `bytesPerRow` alignment for texture↔buffer copies), allocates
  a `MapRead|CopyDst` buffer of `padded_bytes_per_row * height`, does
  `CopyTextureToBuffer` with that padded `bytesPerRow` and
  `rowsPerImage = height`, blocks (own `wait_for`,
  `render_path_tests.cpp:112-114`), then **de-pads on CPU**, row by row,
  into `out` (`render_path_tests.cpp:159-162`).
- `tests/render_path_tests.cpp:560-601`, `readback_texture2d_rgba32f`:
  same shape for `graphics::Texture2D*`.

Dawn's 256-byte `bytesPerRow` rule applies identically to 3D copies (a
3D copy additionally needs `rowsPerImage` per-slice and `Extent3D`
depth — no depth-specific relaxation). A `save_texture3D` should extend
`readback_rgba32f`'s recipe: pad each row to 256 B, `rowsPerImage =
height`, `extent = {width, height, depth}`, then on CPU de-pad **and**
convert f32→f16 while re-packing into upstream's tightly-packed Z-major
layout (§1, §6).

**CopySrc usage on the exportable textures.** All three 3D textures are
created via `graphics::get_texture3D` → `make_texture(e3D, ...)`
(`cpplib/graphics.cpp:373-385,408-431`), whose descriptor already sets
`desc.usage = TextureBinding | StorageBinding | CopySrc | CopyDst`
(`cpplib/graphics.cpp:380-383`) — **`CopySrc` is unconditionally
present**, so no texture-recreation is needed; `CopyTextureToBuffer`
works today. Creation sites: `trail_tex_A`/`trail_tex_B` at
`main.cpp:534-535` (`HALO_COLOR_ANALYSIS`, inactive) and `main.cpp:
538-539` (active), both `Format::R32_FLOAT`,
`GRID_RESOLUTION_X/Y/Z`-sized; `trace_tex` at `main.cpp:543`
(`VELOCITY_ANALYSIS`, inactive) and `main.cpp:546` (active), same.

## 4. Grid geometry needed for export metadata

`config.polyp` parsing: `main.cpp:361-379` — positional `key=value`
pairs (`std::getline(..., '=')` then `>>`, no key validation, order
matters). Fields in order: `NUM_AGENTS`, `GRID_RESOLUTION` (the scalar
knob), `GRID_PADDING`, `SCREEN_X`, `SCREEN_Y`, `CAMERA_FOV`,
`HISTOGRAM_BASE`.

`bin/config.polyp` (cwd for the built binary) values
(`bin/config.polyp:1-7`): Agents 10,000,000; Grid resolution 1024; Grid
padding 0.1; Screen 1800x1000; FOV 30.0; Histogram base 10.0. Everything
below the `## ignored ##` marker (`bin/config.polyp:9`) is dead (parser
stops after 7 reads) — inert alternate `Screen X/Y` presets.

Per-axis grid/world derivation, `main.cpp:418-442`:
- `WORLD_SIZE_X/Y/Z` = dataset bounding-box extent from `_metadata.txt`
  (`main.cpp:398-409,420-422`), padded by `GRID_PADDING * WORLD_SIZE_MAX`
  (`main.cpp:425-429`); `WORLD_CENTER_X/Y/Z` = bbox midpoint
  (`main.cpp:431-433`).
- `GRID_RESOLUTION_X/Y/Z` = `GRID_RESOLUTION` scaled per-axis by that
  axis's fraction of `WORLD_SIZE_MAX`, rounded to the nearest multiple
  of 8 (`nearest_multiple_of`, `main.cpp:220-224`, needed for `/8`
  compute dispatch, e.g. `main.cpp:1138`) — `main.cpp:436-438`.
- `WORLD_SIZE_Y/Z` are then **rescaled** for cubic voxels:
  `WORLD_SIZE_Y = GRID_RESOLUTION_Y * WORLD_SIZE_X / GRID_RESOLUTION_X`
  (`main.cpp:441-442`) — voxel size in Mpc is uniform across axes,
  `= WORLD_SIZE_X / GRID_RESOLUTION_X` (equivalently for Y/Z) — this is
  the number an exporter needs.

World↔grid transform: `world_to_grid`/`grid_to_world`/
`measure_world_to_grid`/`measure_grid_to_world`, `main.cpp:200-218` —
linear maps parameterized by `(world_size_mpc, world_center_mpc,
grid_size_vox)`; `measure_*` are the distance-only (center-independent)
forms used for parameter units.

Active dataset: `REGIME_SDSS` is the only uncommented regime
(`main.cpp:25`); within it, `DATASET_NAME` at `main.cpp:51`
(`"data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=10.3"`) is
the active definition among several alternates. That exact `.bin`/
`_metadata.txt` pair is **not present** under `bin/data/SDSS/` (only
`galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0.*` exist there) — matches
the design doc's note that the VAC-scale catalog still needs packing
from PolyPhy's `sample_3D_linW.csv` (design doc lines 120-123). The
design doc's validation-run grid figure "712×1200×728" (design doc
lines 132-134) is a VAC-run-specific derived value (dataset bbox ×
`GRID_RESOLUTION` knob, via the formula above), not read directly from
`config.polyp`.

## 5. Upstream halos_densities export (F6 block internals)

No separate "export data" block beyond §1 — `git show master:main.cpp`
lines 1071-1119 is one contiguous `if (store_deposit)` block, copied
verbatim into the port at `main.cpp:1177-1226`. Within it:
- Texture saves (`save_texture3D` x2, `main.cpp:1182,1184-1185`) depend
  on the stub (§1) — **currently produce nothing**.
- Metadata text file (`main.cpp:1187-1205`) is pure CPU formatting —
  **already works**.
- Halo-density readback + `halos_measurements.csv`
  (`main.cpp:1207-1223`) depends only on `capture_structured_buffer`
  (§3), fully implemented — **already works**, no `save_texture3D`
  dependency.

The F6 critical-path gap for M5 is narrowly: implement
`graphics::save_texture3D` (2 call sites, 3 texture saves per F6 press —
trail A-or-B, and trace). Everything else in the block already works or
(per §2) is out of scope.

## 6. Half-precision/format quirks

Upstream stores `trail_tex_A/B`/`trace_tex` as `DXGI_FORMAT_R16_FLOAT`
(2 bytes/voxel) by default (`git show master:main.cpp:568-569,574`), and
DirectXTex's `save_texture3D` writes exactly what's in the GPU
texture — **no format conversion**; the `.bin` is raw binary16, tightly
packed, Z-major (§1). No endianness handling exists or is needed
(x86/ARM both little-endian; DirectXTex doesn't byte-swap).

The port cannot use r16float: WebGPU has no `r16float` **storage**
(read_write) format — only `r32float`/`r32sint`/`r32uint` support
`read_write` in WGSL, required by MCPM's racy in-place accumulation
(`textureLoad`+`textureStore`, no atomics). Hence
`graphics::Format::R32_FLOAT` (`main.cpp:538-539,546`; rationale at
`cpplib/graphics.h:15-17` and the resource-mapping table, design doc
lines 80-81). Consequences:
- **Port's raw GPU values are float32, not float16** — `save_texture3D`
  must explicitly convert f32→f16 on CPU before writing the `.bin`, to
  match upstream's byte format/size and keep `OpenPolyphorm.ipynb`,
  pyslime, and skymap's `extractMcpmCube.py` working unmodified (design
  doc lines 125-127 commit to exactly this). No f32→f16 helper exists
  anywhere in `cpplib/` today — new code for M5.
- **For M5 validation** (vs. skymap's cached `mcpm_sdss_d{4,8}.npy`
  tiers, design doc lines 130-148): a converter needs to (a) know the
  `.bin` is f16, Z-major/X-fastest — `export_metadata.txt`'s "simulation
  grid resolution: X x Y x Z [vox]" line (`main.cpp:1192`) gives shape,
  but axis order is implicit/undocumented in the file itself, so the
  converter must hardcode/document the same assumption; (b) `.npy`
  carries its own header (dtype/shape/order), so the converter must
  build one — reading `.bin` as `np.float16`, reshaping to `(Z, Y, X)`
  C-order matches the raw layout; (c) decide whether ×4/×8 block-mean
  downsampling (design doc line 137) happens in f32/f64 to avoid
  compounding f16 quantization error — not settled anywhere in this
  codebase today, needs an explicit M5-design decision.

## Summary: blocking vs. already working

| Piece | Status | Depends on |
|---|---|---|
| `export_metadata.txt` (F6) | works | pure CPU |
| `halos_measurements.csv` (F6) | works | `capture_structured_buffer` (done) |
| `export/deposit.*`, `export/trace.*` (F6) | **stubbed, no-op** | `save_texture3D` (needs 3D readback + f32→f16 + Z-major pack) |
| `export/agents.txt` (F5) | works, **out of M5 scope** | `capture_structured_buffer` (done); design doc defers F5 |
