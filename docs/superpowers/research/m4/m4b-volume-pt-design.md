# M4b design research — volume rendering + path tracing (D3D11/HLSL → Dawn/WGSL)

Scope per the M4b task brief: port `vs_3d`, the five `ps_volume_*` pixel
shaders, `cs_volpath`, and `ps_volpath`; wire real `load_texture2D`; close
the M3→M4 carryovers that touch this code (`m3-carryovers.md` #2–5) and the
M4b-must-handle items in `m4a-carryovers.md`. Read-only research; no code
changed by this document.

**Scope-tension flag (read first):** the top-level port spec
(`docs/superpowers/specs/2026-08-10-macos-webgpu-port-design.md`) lists M4 as
just *"trace volume mode"* and its own Non-goals section explicitly defers
*"Overdensity / highlights / velocity / halo-color modes and the volumetric
path tracer (`cs_volpath`): after validation"* (spec lines 31–33). The M4b
brief that produced this document asks for all of them now. This research
covers the full brief as given (§1–§8 below include every shader), but the
controller should explicitly decide whether M4b actually ships
highlight/overdensity/PT, or whether this document is scoped ahead of the
spec and those shaders get split into a later milestone. See §7/§8 for how
the task list forks either way.

---

## §1 Shader inventory

| HLSL file | lines | entry | draws with | WGSL exists? | vis modes |
|---|---|---|---|---|---|
| `shaders/vs_3d.hlsl` | 42 | `main` (VS) | every `ps_volume_*` | **no** | all `VM_VOLUME*` |
| `shaders/ps_volume_trace.hlsl` | 95 | `main` (PS) | `vs_3d` | **no** | `VM_VOLUME` |
| `shaders/ps_volume_highlight.hlsl` | 76 | `main` (PS) | `vs_3d` | **no** | `VM_VOLUME_HIGHLIGHT` |
| `shaders/ps_volume_overdensity.hlsl` | 69 | `main` (PS) | `vs_3d` | **no** | `VM_VOLUME_OVERDENSITY` |
| `shaders/ps_volume_halocolor.hlsl` | 75 | `main` (PS) | `vs_3d` | **no** | `VM_VOLUME_HALOCOLOR` (`#ifdef HALO_COLOR_ANALYSIS`, **off** in the default build) |
| `shaders/ps_volume_velocity.hlsl` | 58 | `main` (PS) | `vs_3d` | **no** | `VM_VOLUME_VELOCITY` (`#ifdef VELOCITY_ANALYSIS`, **off** in the default build) |
| `shaders/cs_volpath.hlsl` | 455 | `main` (CS) | — | **no** | `VM_PATH_TRACING` (compute half) |
| `shaders/ps_volpath.hlsl` | 59 | `main` (PS) | `vs_2d` (**already ported**, `shaders/vs_2d.wgsl`) | **no** | `VM_PATH_TRACING` (display half) |

Every file in this table exists only as `.hlsl` today (`ls shaders/`
confirmed) — none of the eight has a `.wgsl` sibling, unlike the M3 chain
(`cs_particles_transform.wgsl`, `cs_particles_blit.wgsl`, `vs_2d.wgsl`,
`ps_particles_color.wgsl`, all present). `main.cpp:519-524` declares the
corresponding `VertexShader`/`PixelShader`/`ComputeShader` handles as bare
`= {}` — never loaded — so `graphics::is_ready()` is false for all of them
today and every `VM_VOLUME*`/`VM_PATH_TRACING` draw/dispatch is currently a
guarded no-op (`main.cpp:1336`, `:1369`, `:1400`).

**Nothing main.cpp references is missing from `shaders/`.** All eight files
the vis-mode blocks touch (`main.cpp:1293-1408`) exist in HLSL. No dangling
reference.

**Dead-in-default-build shaders:** `VELOCITY_ANALYSIS` and
`HALO_COLOR_ANALYSIS` are both commented out (`main.cpp:38,40`, active regime
is `REGIME_SDSS`, `main.cpp:25`). Their UI toggles are compiled out too
(`main.cpp:1678-1702`, wrapped in the same `#ifdef`s), so `ps_volume_halocolor`
and `ps_volume_velocity` are unreachable in the shipping configuration. They
still compile against `trail_tex_A/B` (deposit, R32_FLOAT) — same texture
shape as `ps_volume_highlight` — so porting them is cheap if done, but they
gate nothing: no vis-mode toggle can ever select them without also flipping
a `#define` that's out of scope per the top-level spec's REGIME/analysis-mode
non-goals. Recommend: port trivially (same pattern as highlight) for file-
parity, but don't block M4b "done" on them, and don't add draw-path test
coverage for unreachable code.

### Resource/binding table (compute convention: binding==slot, samplers at
16+slot; render convention: texture@2×slot, sampler@2×slot+1 — both already
implemented and load-bearing in `cpplib/graphics.cpp`, confirmed below)

| shader | resource | HLSL register | WGSL binding |
|---|---|---|---|
| `vs_3d` | `ConfigBuffer` (full 336B `RenderingConfig`) | `b4` | `@group(0) @binding(0)` |
| `ps_volume_trace` | `tex_trace` / `tex_trace_sampler` | `t0`/`s0` | `@group(1) @binding(0)` / `(1)` |
| | `tex_false_color` / `tex_false_color_sampler` | `t1`/`s1` | `@group(1) @binding(2)` / `(3)` |
| `ps_volume_highlight` | `tex_trace` / sampler | `t0`/`s0` | `(0)`/`(1)` |
| | `tex_deposit` / sampler | `t1`/`s1` | `(2)`/`(3)` |
| `ps_volume_overdensity` | `tex_trace` / sampler | `t0`/`s0` | `(0)`/`(1)` |
| `ps_volume_halocolor` | `tex_trace` / sampler | `t0`/`s0` | `(0)`/`(1)` |
| | `tex_deposit` / sampler | `t1`/`s1` | `(2)`/`(3)` |
| `ps_volume_velocity` | `tex_trace` / sampler | `t0`/`s0` | `(0)`/`(1)` |
| `ps_volpath` | `tex` / `tex_sampler` (== `display_tex`) | `t0`/`s0` | `(0)`/`(1)` |
| `cs_volpath` | `tex_accumulator` (RW) | `u0` | see §2.6 — **not a direct binding==0 storage-texture port** |
| | `tex_trace` / `tex_trace_sampler` | `t1`/`s1` | `(1)` / `(17)` |
| | `tex_deposit` / `tex_deposit_sampler` | `t2`/`s2` | `(2)` / `(18)` |
| | `tex_palette_trace` / sampler | `t3`/`s3` | `(3)` / `(19)` |
| | `tex_palette_data` / sampler | `t4`/`s4` | `(4)` / `(20)` |

The compute-side binding numbers exactly match `m4a-carryovers.md`'s pinned
contract (17/18/19/20 for s1–s4) — confirmed independently here by reading
`cs_volpath.hlsl:22-30` directly. `cpplib/graphics.cpp:1071-1080` already
implements the `MAX_SLOTS(16) + slot` sampler offset and is exercised by
`tests/render_path_tests.cpp`'s `test_compute_sampler_pairing` (passes today
against a synthetic shader using the same scheme) — this is proven
infrastructure, not a proposal.

The render-side texture/sampler resource *bindings* (2N/2N+1) are likewise
already implemented (`cpplib/graphics.cpp:820-838`) and match every call site
in `main.cpp`'s volume block exactly: `set_texture(&trace_tex, 0)` +
`set_texture_sampler(&tex_sampler_trace, 0)` (`main.cpp:1299-1300`), then
per-mode slot 1 (`palette_trace_tex`/`trail_tex_A|B` at
`main.cpp:1305-1306,1312-1315,1320-1324`). **No graphics.h/.cpp API work is
needed for binding plumbing** — it's already built and test-covered; M4b's
job here is purely writing the WGSL text and the `main.cpp` shader-load /
constant-buffer wiring (§5).

---

## §2 Translation approach per shader

All eight shaders declare a `cbuffer ConfigBuffer : register(b4)` that is a
**prefix subset** of the 336-byte `RenderingConfig` (`main.cpp:263-312`).
Per the M3 convention (translation-notes §1: *"declare the whole shared
struct so one uniform buffer serves every pass"*), every WGSL port here
should declare the **full** 36-scalar struct regardless of which fields the
original HLSL cbuffer stopped at — this repo already has three prior WGSL
ports (`cs_particles_transform.wgsl`, `cs_particles_blit.wgsl`, and by
implication the M2 sim shaders) doing exactly this with zero fixups, since
every member is 4-byte-scalar and never straddles a 16-byte boundary.

One naming footnote, not a bug: `ps_volume_trace.hlsl:25-27` names the
`world_width/height/depth`-offset members `world_X/world_Y/world_Z`, and
`cs_volpath.hlsl:45-47` names the same offset `grid_x/grid_y/grid_z`.
`main.cpp:729-731` sets `rendering_config.world_width/height/depth =
GRID_RESOLUTION_X/Y/Z` — i.e. the field actually always holds the **grid
voxel-count**, not a physical-Mpc size, so `cs_volpath`'s name is the more
accurate one; `ps_volume_trace`'s two aliases are just upstream naming
drift at the same struct offset. Use the canonical `RenderingConfig`
field names (`world_width`/`world_height`/`world_depth`) in every WGSL
struct, matching the M3 precedent of one shared struct definition text
reused verbatim across files.

### 2.1 `vs_3d.hlsl` → `vs_3d.wgsl`

Straight mechanical port, same shape as `vs_2d.wgsl` (already ported,
`shaders/vs_2d.wgsl`) but with a `float3` texcoord and the `texcoord_map`
if/else-if permutation switch (`vs_3d.hlsl:27-39`, six branches over
`{±1,±2,±3}`) ported literally — per the top-level spec's shader-port table,
this switch is explicitly called out to be "ported literally" (spec line
103). No quirks beyond the switch itself; the six branches select which
world axis maps to which texcoord axis for each of the three "most
perpendicular to camera" slab orientations `main.cpp:1338-1357` computes.
Matrix multiply order: `mul(projection, mul(view, mul(model, position)))`
→ `cfg.projection * (cfg.view * (cfg.model * input.position))` — same
column-major/column-vector convention already cross-checked and resolved
by the M3 notes §1 (no transpose needed anywhere in this port).

Declares `@group(0) @binding(0)` (has a cbuffer) — `uses_group0 = true`.
No `@group(1)` bindings (no textures/samplers in the vertex stage).

### 2.2 `ps_volume_trace.hlsl` → `ps_volume_trace.wgsl`

Two textures (`tex_trace` 3D, `tex_false_color` 2D palette), trim-box
early-out, `remap()` helper (`1.0 - exp(-slope*val)`), and the
`fragment.rgb *= 2.0;` "compensate for drawing 1 stack instead of 3"
comment at the end of every `ps_volume_*` shader (`:93`) — a genuine
QUIRK, kept per the "never fix upstream math" rule; tag
`// QUIRK(single_stack_2x_compensation): kept for VAC parity` in the port.
A large commented-out "Proxy draws config" block (`:57-90`, sightline/shell
visualization debug code, never compiled) should be ported as a comment
verbatim for 1:1 file correspondence — same precedent as M3's dead-RNG-code
item (translation-notes T11: "kept for 1:1 file correspondence... do not
clean up").

`Texture3D.Sample()` (implicit trilinear via the bound sampler, no LOD
argument) → WGSL `textureSample()` on `texture_3d<f32>` — this is a
**fragment-shader-only** construct (`textureSample` requires
non-uniform-control-flow derivatives, i.e. must be called unconditionally
per WGSL's derivative-uniformity rules; both `ps_volume_trace` and its
siblings call it unconditionally at the top of `main()` before any
branch, matching the HLSL's own unconditional-sample-before-branch shape
at `:45`/`:49-51` — safe, no restructuring needed). Requires the
`float32-filterable` device feature (per the top-level spec's GPU mapping
table) since `tex_trace`/`tex_deposit` are `r32float` — already a stated
M1 requirement, not new to M4b.

No OOB-load guard needed: `TRIANGLESTRIP`/`TRIANGLELIST` fragment coverage
from `super_quad_mesh`'s clip-space quads never produces a texcoord outside
`[0,1]³` by construction (the mesh is built to tile `[-1,1]` in XY, mapped
through `vs_3d`'s texcoord passthrough), and out-of-`[0,1]` texcoords would
be handled by the sampler's `CLAMP` address mode (`tex_sampler_trace =
get_texture_sampler(graphics::CLAMP, ...)`, `main.cpp:547`) identically in
both APIs — a genuine "no divergence" case, unlike the compute-side OOB
questions elsewhere in this port.

### 2.3 `ps_volume_highlight.hlsl` → `ps_volume_highlight.wgsl`

Same shape as 2.2 plus a `tex_deposit` sample and a `histogram_base`-scaled
smoothstep window (`:60-71`) highlighting a density band in green. No new
translation risk beyond what 2.2 already covers — literal port.

### 2.4 `ps_volume_overdensity.hlsl` → `ps_volume_overdensity.wgsl`

Single texture, three-way density-bucket color select via `#define
COLOR_UNDERDENSE/MIDDENSE/OVERDENSE` macros (`:37-39`) — WGSL has no
preprocessor macros; port these as `const` vec4 module-level declarations
(`const COLOR_UNDERDENSE = vec4<f32>(0.0, 0.0, 25.0, 0.08);`, etc.) — purely
mechanical, WGSL `const` supports this. One odd-looking but faithful line:
`fragment.rgb *= remap(2.71, sample_weight);` (`:63`) — `2.71` is a literal
constant (≈ e), not a variable; the shader always remaps the *same* input
regardless of `trace`, so this term is a flat brightness scalar depending
only on `sample_weight`. Preserve verbatim; do not "fix" into something
trace-dependent even though it looks like it should be.

### 2.5 `ps_volume_halocolor.hlsl` / `ps_volume_velocity.hlsl` (dead-in-default-build, §1)

Both are literal ports of the same shape (finite-difference gradient of
`tex_trace` for halocolor at `:51-57`; a `.gba`-channel unpack of a 4-channel
`tex_trace` for velocity at `:45,52`, implying `VELOCITY_ANALYSIS` widens
`trace_tex` to `R32G32B32A32_FLOAT` per `main.cpp:538`'s `#ifdef` branch,
consistent with `ps_volume_velocity.hlsl:45`'s `.rgba` sample). Recommend
porting these for file-parity (cheap, same risk profile as 2.2–2.4) but
explicitly **not** wiring `main.cpp`'s `#ifdef HALO_COLOR_ANALYSIS` /
`#ifdef VELOCITY_ANALYSIS` blocks live, and not adding draw-path test
coverage — see §1.

### 2.6 `cs_volpath.hlsl` → `cs_volpath.wgsl` — the hard problem

**`tex_accumulator` is `RWTexture2D<float4>` (`cs_volpath.hlsl:22`), read
AND written in the same invocation** (`:448-451`, the `TEMPORAL_ACCUMULATION`
running-average: `current_value = tex_accumulator[pixel_xy]; tex_accumulator
[pixel_xy] = current_value * n/(n+1) + new/(n+1)`). This is a genuine
read-modify-write on a **4-channel float storage resource**, bound to
`display_tex` (`Format::RGBA32_FLOAT`, `main.cpp:543,1372`).

This repository has already hit and adjudicated the underlying WGSL
constraint, independently of M4b: `cs_agents_propagate.wgsl:118`'s own
comment states plainly *"`read_write` — which in WebGPU restricts us to
r32uint/r32sint/r32float"* — i.e. **`read_write` storage-texture access in
this build is only available for single-channel 32-bit formats.**
`cs_particles_blit.wgsl:121` independently confirms the other half of the
constraint: its `rgba32float` storage texture is declared `write`-only
(`texture_storage_2d<rgba32float, write>`), never `read_write`. There is no
`texture_storage_2d<rgba32float, read_write>` available to a literal port
of `cs_volpath.hlsl:22`.

This is the **same class of problem as M3's `InterlockedAdd`-on-texture
blocker** (translation-notes §0.1: T1, "the hard problem"), not the same
mechanism — no atomics needed here (each compute invocation owns exactly
one `pixel_xy`, so there's no cross-thread race, only a same-thread
read-then-write that a plain non-atomic access handles correctly) — but the
same *resource-shape* mismatch: the D3D11 resource type has no faithful
single-resource WGSL equivalent for what this shader does with it.

**Two options:**

| option | mechanism | cost | fidelity |
|---|---|---|---|
| **(a) RECOMMENDED** | Convert `tex_accumulator` to a `var<storage, read_write> tex_accumulator: array<vec4<f32>>` buffer, row-major `y*width+x` (same indexing scheme as M3's particle-splat buffer, translation-notes §0.1) at `@group(1) @binding(0)`. Add one new tiny compute shader (no HLSL counterpart — new infrastructure, same class as M3's atomic buffer) that copies the buffer into `display_tex` (`texture_storage_2d<rgba32float, write>`) each frame, mirroring `cs_particles_transform`→`cs_particles_blit`'s two-dispatch shape exactly. `ps_volpath` then samples `display_tex` completely unchanged. | +1 new buffer resource (`window_width*window_height*16` bytes, vec4), +1 new tiny WGSL file, +1 dispatch/frame, +`main.cpp` bind-block edit | **exact** — plain non-atomic buffer RMW is safe here (one thread per pixel, no collisions), identical numeric result to the D3D11 UAV RMW |
| (b) rejected | Split into 4× `texture_storage_2d<r32float, read_write>` "planar" textures (one per channel) sampled/packed by `cs_volpath` directly, then a pack pass merges the 4 planes into `display_tex` for `ps_volpath` to sample. | 4 new texture resources + 1 new pack shader + 4x the bind-group entries in `cs_volpath`'s own `@group(1)` | Also exact, but doubles the binding surface (5 resources become 8) and introduces a *new* multi-resource pattern instead of reusing the buffer-accumulator pattern M3 already established and tested — no advantage over (a), strictly more surface area |

**Recommendation: (a).** It reuses the exact "accumulate into a buffer,
blit into the sampled texture" shape M3 already built, tested
(`test_particle_chain_pixels_exist`), and got the human visual gate on —
same risk profile, not a new pattern. The new blit shader is trivial (one
`textureStore(display_tex, coord, accum[idx])` line) and independently
compile-testable. Flag prominently in the port: **this is the single
largest M4b implementation task**, on par with M3's T1/§0.1 in scope (a
host-side resource-shape change, not just shader text), and it must land
*before* `cs_volpath`'s WGSL can be written against a real binding table.

**OOB guard question ties directly to the PT-dispatch-rounding decision**
(§5, carryover #2): with option (a), if the dispatch is ever rounded up to
cover a partial tile, the extra invocations' `pixel_xy` will exceed
`width`/`height`, and a WGSL storage-buffer OOB index **clamps** (into a
real, wrong buffer slot) rather than **discarding** (D3D11 typed-UAV OOB
write semantics) — the exact T2 guard class from the M3 notes. See §5 for
the recommendation (round up + add the guard).

RNG (`RNG` struct, `:75-111`, xorshift-style `random_uint`/`random_float`,
`wang_hash`) is a literal, mechanical port — same shape as M2's RNG (design
spec: *"identical `wang_hash`/xorshift RNG constants"*), just an inline
`struct`-with-methods here instead of free functions; WGSL has no member
functions, so this becomes free functions taking/returning the two `u32`
seed fields by value/pointer (`fn rng_random_float(rng: ptr<function, RNG>)
-> f32`, etc.) — mechanical, no semantic risk. `#ifdef`-style feature flags
at the top of the file (`TEMPORAL_ACCUMULATION`, `RUSSIAN_ROULETTE`,
`HALO_ILLUMINATION`, `TRACE_ILLUMINATION`, with `GRADIENT_GUIDING`/
`TRACE_SHARPENING`/`WHITESKY_ILLUMINATION`/`POINT_ILLUMINATION` disabled)
have no WGSL preprocessor equivalent; either hardcode the currently-enabled
set directly into the ported control flow (simplest, matches "what actually
ships") or use WGSL `override` booleans + `@id` pipeline-overridable
constants if toggling from the host is ever wanted — recommend hardcoding
for M4b (no evidence any of these flags are runtime-toggled; they're
compile-time-only in the original too) and note the disabled branches as
dead code, same "kept for 1:1 correspondence" treatment as M3's RNG.

`numthreads(10,10,1)` = 100 threads/workgroup — well under any device limit
(unlike M2/M3's `numthreads(10,10,10)`=1000 concern, already de-risked per
translation-notes T13); no blocker here.

### 2.7 `ps_volpath.hlsl` → `ps_volpath.wgsl`

Trivial: one texture sample, ternary tonemap-or-passthrough based on
`compressive_accumulation`. Pairs with the **already-ported** `vs_2d.wgsl`
(`shaders/vs_2d.wgsl`, from M3) — `ps_volpath` is the only new file needed
here, no new vertex shader. One footnote for the port, not a bug: the
HLSL cbuffer's last two members are named `dummy2`/`dummy3` (int) instead of
`guiding_strength`/`scattering_anisotropy` (float) — same byte offsets,
unused by this shader either way; use the canonical `RenderingConfig` field
names/types in the WGSL struct per the shared-struct convention (§2 intro),
not the HLSL's locally-inconsistent names.

**This shader declares a cbuffer** (`ps_volpath.hlsl:9-50`) — so it *does*
need `@group(0)`, even though its paired vertex shader (`vs_2d.wgsl`) does
not (confirmed: `vs_2d.wgsl` has no cbuffer, translation-notes V9). Since
`draw_mesh`'s `needs_group0 = g_vertex_shader->uses_group0 ||
g_pixel_shader->uses_group0` (`graphics.cpp:807`), this pairing needs a bound
constant buffer even though the *vertex* stage doesn't declare one — see §5
for why this is a **fourth** missing `set_constant_buffer` site, beyond the
three volume draws the carryovers already name.

---

## §3 The 28-byte stride vertex path

`main.cpp:694-696` builds `super_quad_mesh` with `super_quad_vertices_stride
= sizeof(float) * 7` = 28 bytes (`float4 position` + `float3 texcoord`,
matching `vs_3d.hlsl:1-5`'s `VertexInput` exactly). `cpplib/graphics.cpp`'s
`fill_vertex_attributes()` **already has a `case 28:`** branch
(`graphics.cpp:686-691`: `Float32x4` at offset 0 + `Float32x3` at offset 16,
`shaderLocation` 0/1) — the vertex-layout table entry m3-carryovers.md #5
flagged is already present and correct by inspection. What's missing is
**draw coverage**: nothing in `tests/render_path_tests.cpp` currently
constructs a 28B-stride `Mesh` and calls `draw_mesh` on it (only the 24B
`quad_vertices` path is exercised, via `QUAD_VS_WGSL`/`SAMPLE_PS_WGSL` and
`test_particle_chain_pixels_exist`'s `quad_vertices_stride = 24`).

Recommended test (independently addable once `vs_3d.wgsl` exists, §7 task
list): a small offscreen-RT draw using `vs_3d.wgsl` + a trivial
pass-through pixel shader (or `ps_volume_trace.wgsl` once ported) against a
**single** `super_quad_vertices_template`-shaped quad (not the full
`GRID_RESOLUTION`-deep stack — one 6-vertex/28B-stride quad is enough to
pin the vertex-layout wiring), asserting the draw doesn't fatal and that a
known-position texel reads the expected sampled color. This closes
m3-carryovers.md #5 and doubles as `vs_3d.wgsl`'s first real-draw
regression test.

---

## §4 Palette TGA loading

`cpplib/graphics.cpp:464-471`'s `load_texture2D` is a 1×1 white stub
(`warn_once("load_texture2D")`); every `COLOR_PALETTE_TRACE`/
`COLOR_PALETTE_DATA` macro in `main.cpp` (e.g. `"data/palette_sunset3.tga"`,
`main.cpp:52-53` and eleven other `REGIME_*`-conditional pairs) is a
**relative path**.

**File format, verified by inspection of all 15 TGA files present at
`bin/data/*.tga`** (Python struct-unpack of each 18-byte header):

- Every palette is TGA image-type **2** (uncompressed truecolor) — no RLE
  (`imgtype 10`) anywhere, no color-mapped (`imgtype 1`) anywhere.
- Every palette is **24 bpp** (BGR byte order, no alpha channel) — no 32bpp
  variant exists.
- Every palette's image-descriptor byte has origin bits `00` — **bottom-left
  origin** (TGA convention: scanlines stored bottom-to-top in the file).
- Sizes vary (e.g. `palette_hot.tga` 130×16, `palette_vaneyck_red.tga`
  620×81) — every shader samples these at `float2(remap(t,1.0), 0.5)`, i.e.
  **always the middle row** (`ps_volume_trace.hlsl:55`,
  `cs_volpath.hlsl:196,200`), so they function as 1D color ramps baked into
  a 2D image; the vertical-flip direction is therefore currently
  unobservable in practice (only matters if a future palette or sampling
  point departs from `y=0.5`), but should still be decoded correctly for
  correctness-in-general.

This is a single, uniform, simple format — no branching needed for RLE,
color-mapped, or 32bpp variants. The top-level spec's own plan
(*"stb_image (vendored, single header) replaces DirectXTex's
`LoadFromTGAFile`"*, spec line 59) is directly applicable: **`stb_image.h`
is not currently vendored anywhere in this repo** (`find` for
`stb_image*` only turns up Dawn's third-party `stb_image_write.h`, unrelated
and not linked into `polyphorm`) — vendoring it is real, if small, M4b
work. `stbi_load()` natively supports TGA type 2 at any bit depth including
24bpp, auto-flips to conventional top-down row order regardless of the
source origin bit, and returns raw `RGB`/`RGBA` bytes ready to hand to
`graphics::get_texture2D()` (which already accepts a raw `data` pointer +
`Format`, `graphics.cpp:383-402`) after a 3-channel→4-channel expand
(`stbi_load(..., &n, 4)` requests RGBA directly, one call).

**Format choice (open, low-risk, flag for review):** `Format::RGBA8_UNORM`
is the closer match to the source data (plain 24bpp truecolor, no evidence
D3D11's `LoadFromTGAFile` applied an sRGB reinterpretation to a color-ramp
LUT texture) vs. `Format::RGBA8_UNORM_SRGB` (reserved, per its own comment
at `graphics.h:26`, for the *window surface's* format quirk specifically,
not general textures) or `Format::RGBA32_FLOAT` (what the white-stub
currently returns, unnecessarily wide for an 8-bit-source LUT).
**Recommend `RGBA8_UNORM`** as the default hypothesis; flag for a visual
check against the README's palette description / any reference screenshots
once real rendering is live — this is a M4 human-visual-gate item, not a
scientific-validation item (see §6 on why palette-exact-color mismatches
don't gate M5).

**CWD discipline — a real, currently-latent gap.** `SHADER_ROOT` is an
**absolute** CMake-injected path (`CMakeLists.txt:95`,
`target_compile_definitions(polyphorm PRIVATE
SHADER_ROOT="${CMAKE_SOURCE_DIR}/shaders")`), so shader loading is already
CWD-independent. The palette path is not: it's a bare `"data/..."` literal,
resolved by `fopen()` relative to the process's current working directory
(`cpplib/file_system.cpp:9`). The repo's upstream `bin/` directory (checked
into git, Windows-era layout: `bin/data/*.tga`, `bin/config.polyp`) is where
this worked historically (run from `bin/`). But the M3 human-visual-gate
run recipe (`m3-carryovers.md`: `cd /tmp/polyviz && <repo>/build/polyphorm
--dataset /tmp/polyviz/testdata`) changes CWD to a directory with **no**
`data/` subfolder — once `load_texture2D` becomes real, that exact recipe
will fail to find the palette TGAs. Recommend mirroring `SHADER_ROOT`'s
fix: add a `DATA_ROOT="${CMAKE_SOURCE_DIR}/bin/data"`-style absolute define
and rewrite the `COLOR_PALETTE_TRACE`/`COLOR_PALETTE_DATA` macros (or the
`load_texture2D` call sites) to use it, eliminating the CWD footgun
entirely rather than documenting a "must `cd` here" discipline that the
existing gate recipe already violates.

---

## §5 main.cpp integration

**Per-draw `set_constant_buffer` gap — four sites, not three.** The M3/M4a
carryovers name *"the three volume draw_mesh calls"* (`main.cpp:1344,1350,
1356`, each preceded only by `update_constant_buffer`, never
`set_constant_buffer`) — confirmed by direct read: none of the three has a
`set_constant_buffer(&rendering_settings_buffer, 0)` call in the block
(contrast `VM_PARTICLES`'s compute dispatches, which correctly call it at
`main.cpp:1262,1278`). **A fourth site has the identical gap and is not
named in the existing carryover text**: `VM_PATH_TRACING`'s final
`draw_mesh(&quad_mesh)` at `main.cpp:1405`, pairing `vertex_shader_2d`
(no cbuffer, `uses_group0=false`) with `ps_volpath` (**has** a cbuffer,
§2.7 — so `needs_group0` evaluates true via the OR in `graphics.cpp:807`)
— and the block preceding it (`main.cpp:1399-1407`) never calls
`set_constant_buffer` either. All four sites hit `draw_mesh`'s loud
`fatal("draw_mesh: shader declares @group(0)...")` (`graphics.cpp:810`) the
first frame real shaders replace the `= {}` stubs, unless fixed together.
Land one `set_constant_buffer(&rendering_settings_buffer, 0)` call
immediately before each of the four `draw_mesh` calls.

**`is_ready` gates are already correctly wired — no removal needed.**
`docs/superpowers/research/m4/imgui-integration-design.md` (M4a) already
surveyed every volume/PT draw/dispatch site and landed the gating
(`main.cpp:1336`: `is_ready(&vertex_shader) && selected_ps &&
is_ready(selected_ps)`; `:1369`: `is_ready(&cs_volpath)`; `:1400`:
`is_ready(&ps_volpath)`) with comments already reading `// M4b: real shader
lands, gate stays as belt-and-suspenders`. These gates will self-activate
the instant `main.cpp:521-524`'s `= {}` stubs are replaced with real
`load_vs`/`load_ps`/`load_compute` calls (same lambda pattern already used
for `vertex_shader_2d`/`pixel_shader_2d`, `main.cpp:497-516`) — no
conditional-removal work is needed in M4b, just: (1) load the real shaders,
(2) load real palette textures (§4), (3) add the four constant-buffer binds
above.

**`reset_pt` on resize — confirmed still missing.** Direct read of the
resize block (`main.cpp:871-918`) shows `depth_buffer`, `display_tex`, and
`display_accum_buffer` are recreated and `rendering_config.screen_width/
height` updated, but no `reset_pt = true;` anywhere in the block. Per
carryover #1: one line, inside the `if (!window_minimized && (...))` branch,
anywhere after the resize is detected. Land it WITH this port (`cs_volpath`
reads `pt_iteration`, and a resize recreates `display_tex` — the PT
accumulator's new home per §2.6 — uninitialized while `pt_iteration` stays
nonzero, corrupting the running average with garbage until a manual F2/
reset).

**PT dispatch round-up decision.** `main.cpp:1386-1389`:
```
graphics::run_compute(
    rendering_config.screen_width / int(PT_GROUP_SIZE_X),
    rendering_config.screen_height / int(PT_GROUP_SIZE_Y),
    1);
```
Integer division truncates — a `screen_width` not a multiple of 10 leaves a
dead strip of pixels along the right/bottom edge that `cs_volpath` never
dispatches into. `cs_volpath.hlsl` itself has **no internal bounds check**
on `pixel_xy` (`:350`, taken directly from `dispatchThreadId.xy`, no
`if`-guard anywhere in `main()`) — D3D11 simply never launches threads
beyond the requested group count, so the original never produces
out-of-range `pixel_xy` values regardless of dispatch shape; the truncation
artifact is purely "some pixels never get written," not an OOB-safety
issue upstream.

Two options:

1. **Keep truncation (bug-for-bug).** Zero shader change. Visible dead
   band grows as window size increasingly deviates from a multiple of 10 —
   this was a latent, rarely-hit upstream quirk on a fixed-size Windows
   window, but M4a's resizable-window support (already landed,
   `m4a-carryovers.md` intro) makes arbitrary window sizes the *common*
   case, not an edge case.
2. **Round up** (`(screen_width + PT_GROUP_SIZE_X - 1) / PT_GROUP_SIZE_X`,
   same for Y) **and add an explicit bounds guard** in `cs_volpath.wgsl`
   (`if (pixel_xy.x >= u32(cfg.screen_width) || pixel_xy.y >=
   u32(cfg.screen_height)) { return; }`, first line of `main()`) — required
   *only if* option 2 is chosen, and required for a reason: §2.6's
   buffer-based accumulator has clamp-not-discard OOB semantics (the exact
   T2 guard class from the M3 notes), so the extra tail-band invocations
   from rounding up would otherwise silently corrupt a real buffer slot
   every frame instead of being harmlessly discarded the way an
   out-of-range D3D11 UAV write would be.

**Recommendation: option 2 (round up + guard).** This is a platform-
behavior-divergence guard, exactly the kind the port's stated
quirk-preservation policy carves out ("platform-behavior divergences...
get explicit guards to reproduce D3D11 semantics") — the guard exists
*because* the buffer conversion (an unavoidable, already-adjudicated
platform difference, §2.6) would otherwise introduce a *new*, worse bug
(silent per-frame corruption of one real pixel) in exchange for fixing a
cosmetic one. Still: **this changes visible behavior from upstream** (no
more dead edge band) and should get its own explicit sign-off line in
whatever plan implements it, per the carryover's instruction — it is not
a "no behavior change" mechanical port.

---

## §6 Verification plan

**Headless-testable, recommended additions:**

- **Shader compile suite** (`tests/shader_compile_tests.cpp` +
  `CMakeLists.txt`'s `SHADER_DIR` list): add `vs_3d.wgsl` (expect
  `uses_group0=1`), `ps_volume_trace.wgsl`, `ps_volume_highlight.wgsl`,
  `ps_volume_overdensity.wgsl` (all expect `uses_group0=1`), `cs_volpath.wgsl`
  and the new blit shader (§2.6) (`uses_group0=1`/`0` respectively), and
  `ps_volpath.wgsl` (`uses_group0=1`). Cheap, catches every "uncertain line"
  candidate at `ctest` time before app startup — same rationale the file's
  own header comment states for the existing entries.
- **28B-stride draw test** (§3): new `render_path_tests.cpp` test, single
  `super_quad_vertices_template`-shaped quad through `vs_3d.wgsl` +
  a trivial or `ps_volume_trace.wgsl` pixel shader into an offscreen RT,
  pinned readback value. Closes m3-carryovers.md #5.
- **Volume draw readback test**: extend the above (or a sibling test) with
  known `tex_trace`/`tex_false_color` contents and assert the composited
  pixel color matches a hand-computed expected value through `remap()` —
  same "pixels-exist-with-a-pinned-value" pattern as
  `test_particle_chain_pixels_exist`.
- **`cs_volpath` headless dispatch test**: small screen (e.g. 20×20 = a
  2×2 grid of `10×10` workgroups), synthetic `tex_trace`/`tex_deposit`/
  palette textures, one iteration, readback the accumulator buffer (§2.6
  option a) and assert plausible structure (center ray through a nonzero-
  trace voxel produces nonzero radiance; a ray that misses the AABB entirely
  reads back the sky value, `float3(0,0,0)` per `#ifdef WHITESKY_ILLUMINATION`
  being off). This is the highest-value new test in this milestone — it
  pins both the accumulator-buffer conversion (§2.6) and the OOB-guard
  decision (§5) without needing the interactive app or a real dataset.
- **Blend/screenshot-compare**: `blend_state.alpha` formula
  (`graphics.cpp:723-727`) is a documented guess, unobservable in M3 (alpha
  always 1.0 there) and load-bearing the moment volume slabs vary alpha per
  fragment. **No D3D11 reference screenshot exists in this repo** to diff
  against — this cannot be closed headlessly. Note, though: the top-level
  spec's own risk register says *"Slice-stack rendering artifacts under
  WebGPU blending: cosmetic only; validation reads the exported cube, not
  pixels"* (spec, Risks section) — so this uncertainty does **not** gate
  the M5 validation bar, only the human-facing visual quality of the
  interactive volume view. Recommend: ship the documented-guess formula,
  defer a real fix to if/when a D3D11 reference capture becomes available,
  and don't spend M4b test-authoring effort trying to pin an unverifiable
  number.

**Needs the human visual gate (cannot be headlessly verified even in
principle):** actual on-screen appearance of `VM_VOLUME`/`VM_VOLUME_
HIGHLIGHT`/`VM_VOLUME_OVERDENSITY` against the synthetic test dataset
(same recipe as the M3 gate); PT accumulation converging/denoising visibly
over iterations; resize-during-PT-accumulation not showing a garbage frame
(pins the `reset_pt` one-liner, §5); palette color-ramp appearance
matching the intended look (§4's `RGBA8_UNORM` hypothesis).

**Cannot be verified at all without the real SDSS VAC dataset:** nothing
in M4b's actual scope requires it — M4b's own correctness (pipeline wiring,
binding contracts, dispatch shapes) is fully exercisable on the synthetic
`gen_test_dataset` grid, same as M2/M3. The VAC-scale validation
(log-trace Pearson ≥ 0.9 at d8) is M5's gate, reads the *exported cube*
(not rendered pixels — spec's own text again), and is explicitly out of
this milestone's scope; M4b should not attempt to pre-validate against it.

---

## §7 Task decomposition sketch

Two independent tracks after a shared prerequisite; PT track (9–11) has no
dependency on the volume-mode track (2–8) beyond both needing (1)'s
constant-buffer-bind fix pattern, so they can run in parallel once (1)–(2)
land.

0. **(Prerequisite, tiny)** Land the four `set_constant_buffer` calls (§5)
   and the `reset_pt = true;` resize one-liner (§5) — pure `main.cpp`
   edits, no shader dependency, testable today by wrapping any trivial
   `@group(0)`-declaring shader pair through `draw_mesh` (the existing
   Test 3 gap in `render_path_tests.cpp`, currently only documented, not
   run, per its comment at `:365-378` — could finally be made a real death-
   test-adjacent positive-path check here, or left as a code-review item).
1. **Real `load_texture2D`** (§4): vendor `stb_image.h`, decode TGA →
   RGBA8, `DATA_ROOT` absolute-path fix, replace the stub. Independently
   testable: load a real palette TGA, assert dimensions + a known pixel's
   BGR→RGBA-converted value.
2. **`vs_3d.hlsl` → `.wgsl`** (§2.1). Add to shader-compile suite.
3. **28B-stride draw test** (§3, §6). Depends on (2).
4. **`ps_volume_trace.hlsl` → `.wgsl`** (§2.2) + volume-draw readback test
   (§6). Depends on (2).
5. **`main.cpp` VM_VOLUME wiring**: replace `vertex_shader`/`pixel_shader`
   `= {}` stubs with real loads (§5's gate self-activates). Depends on
   (0),(1),(2),(4). Human visual gate: `VM_VOLUME` on the synthetic
   dataset.
6. **`ps_volume_highlight.hlsl` / `ps_volume_overdensity.hlsl` → `.wgsl`**
   (§2.3, §2.4). Depends on (2). Can run parallel to (4)/(5).
7. **`main.cpp` VM_VOLUME_HIGHLIGHT / VM_VOLUME_OVERDENSITY wiring**.
   Depends on (0),(1),(6). Human visual gate.
8. **(Scope-gated, see §1/§8) `ps_volume_halocolor.hlsl` /
   `ps_volume_velocity.hlsl` → `.wgsl`**, file-parity only, no `main.cpp`
   wiring (unreachable without flipping out-of-scope `#define`s). Lowest
   priority; candidate to drop entirely if the controller resolves the
   scope-tension flag toward "M4b stays trace-only."
9. **`cs_volpath` accumulator-buffer conversion** (§2.6) — the largest
   task. Sub-decompose if needed: 9a) write `cs_volpath.wgsl` against the
   buffer-based accumulator + the new blit shader, add both to the compile
   suite; 9b) the headless dispatch test (§6). No dependency on the
   volume-mode track.
10. **`ps_volpath.hlsl` → `.wgsl`** (§2.7). Pairs with the already-existing
    `vs_2d.wgsl` — no new vertex shader. Independent of (9), can run in
    parallel.
11. **`main.cpp` VM_PATH_TRACING wiring**: real loads for `cs_volpath`/blit/
    `ps_volpath`, the round-up-dispatch decision (§5, needs sign-off),
    the fourth `set_constant_buffer` site (§5, item 0 should already cover
    this if sequenced first). Depends on (9),(10). Human visual gate:
    PT accumulates/denoises; resize doesn't leave garbage (pins task 0's
    `reset_pt` fix in the PT-specific case).
12. **Bookkeeping**: `QUIRK()` comments on every preserved-quirk line (§2's
    per-shader notes), mark closed items in `m3-carryovers.md`/
    `m4a-carryovers.md`, open a post-validation cleanup ticket for each
    preserved quirk per the top-level spec's stated convention.

---

## §8 Risk register

| # | risk | likelihood | impact | mitigation |
|---|---|---|---|---|
| 1 | `cs_volpath`'s `RWTexture2D<float4>` RMW has no direct WGSL `read_write` storage-texture equivalent (confirmed via this repo's own `read_write`-restricted-to-r32* precedent) | **certain** (confirmed by reading, not a guess) | **high** — blocks `VM_PATH_TRACING` entirely without a resource-shape redesign | Adopt the buffer+blit pattern (§2.6 option a), which reuses M3's already-tested atomic-buffer precedent; size the task accordingly in planning (§7 task 9 is the biggest single task in this milestone) |
| 2 | PT dispatch round-up (if chosen) requires a new OOB guard in `cs_volpath.wgsl`; forgetting it silently corrupts one real pixel's accumulator slot every frame | medium (easy to land the round-up half without the guard half) | medium (per-frame visible pixel corruption at the tail band, gets worse with more resizes) | Land round-up and the guard in the same change; the headless dispatch test (§6) with a non-multiple-of-10 screen size directly pins this |
| 3 | Four `set_constant_buffer` sites (not three — §5 found a fourth at the `ps_volpath` quad draw) must all land together or `draw_mesh` fatals the first frame real shaders load | high (easy to miss the fourth, not named in existing carryover text) | high but immediately loud (a `fatal()`, not silent corruption — fails fast, not a quiet bug) | This document names all four explicitly; §7 task 0 sequences the fix first, before any real shader lands |
| 4 | Blend alpha-channel formula is an unverified guess, now load-bearing (volume alpha varies per-fragment, unlike M3) | medium (visibly "off" contrast/transparency vs. upstream is plausible) | **low for M5** (spec: validation reads the exported cube, not pixels), medium for the human-facing interactive view | Ship the documented guess; defer a real fix pending a D3D11 reference capture; don't over-invest test effort here (§6) |
| 5 | Palette TGA color-space/format choice (`RGBA8_UNORM` vs `_SRGB`) unverified against upstream's actual D3D11 decode | medium | low-medium (visible color-tone shift, not a validation blocker — same "cosmetic, pixels not read by M5" argument as #4) | Use `stb_image` (matches the top-level spec's stated plan) decoding to plain `RGBA8_UNORM`; flag for the human visual gate |
| 6 | **Scope tension**: this document's brief includes shaders (`highlight`/`overdensity`/`halocolor`/`velocity`/`cs_volpath`/`ps_volpath`) the top-level spec's own Non-goals section defers to "after validation" | certain (textual conflict confirmed by reading both documents) | **process risk**, not a technical one — wasted implementation effort if the controller intended M4b to stay trace-only, or a stale/outdated spec section if M4b's expanded scope is the current intent | Flagged explicitly here (top of document and §1); needs an explicit controller decision before task-list execution starts, not a research-time guess |
| 7 | `ps_volume_halocolor`/`ps_volume_velocity` are dead code in the shipping `REGIME_SDSS` build (`#ifdef`-gated off) | certain | low (wasted effort only if ported+wired; near-zero if ported for file-parity only, per §7 task 8's recommendation) | Don't build `main.cpp` wiring or test coverage for these two; file-parity port only, and only if time permits |
| 8 | `is_ready` gate design was already fully solved by M4a (`imgui-integration-design.md`) — risk of redundant rework if a future implementer re-derives it | low (this document verifies and cites the existing state directly) | low (wasted time only) | §5 states explicitly: no gate-removal work needed, just real shader loads |
