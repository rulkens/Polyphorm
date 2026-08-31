# HLSL → WGSL translation notes — M3 particle-rendering chain

Reference material for the M3 plan (particle rendering / `VM_PARTICLES`).
Covers the four shaders that make up the chain:

| source (repo) | draft | lines |
|---|---|---|
| `shaders/cs_particles_transform.hlsl` (104 lines) | `cs_particles_transform.wgsl` | 395 |
| `shaders/cs_particles_blit.hlsl` (40 lines) | `cs_particles_blit.wgsl` | 188 |
| `shaders/vs_2d.hlsl` (21 lines) + `shaders/ps_particles_color.hlsl` (18 lines) | `vs_2d_ps_particles_color.wgsl` | 145 |

None of the drafts have been compiled — no Dawn/Tint binary is reachable
from this sandbox. Validation was done by a line-by-line walk against the
HLSL and against this fork's already-working M2 drafts and M2b-shipped
`main.cpp`/`cpplib/graphics.{h,cpp}`; see §7 for what remains genuinely
uncertain.

This document follows the house style of
`docs/superpowers/research/m2/wgsl-drafts/translation-notes.md`: difference
tables with a risk column (**H** = can silently change validation/visual
results, **M** = needs a deliberate decision or a device capability, **L** =
mechanical, verifiable by inspection), quirks preserved bug-for-bug (never
"fixed"), and every RNG/atomic draw kept in its original order.

---

## 0. The two findings that drive every other decision in this chain

### 0.1 `InterlockedAdd` on `RWTexture2D<uint>` has no WGSL equivalent

`cs_particles_transform.hlsl:99,102` do `InterlockedAdd(tex_out[out_pos], N)`
— an **atomic read-modify-write on a texture UAV texel**. D3D11 supports
this for `R32_UINT`/`R32_SINT` typed UAVs. WGSL has **no atomic storage-
texture type at all**; `atomic<T>` only exists for `var<storage>` and
`var<workgroup>` (buffers), never `var<storage> : texture_storage_*`.

This is the hardest problem in the M3 chain, harder than anything M2 hit,
because unlike `cs_agents_propagate`'s preserved-race deposit RMW (M2 notes
§3, A3 — a real race, kept deliberately, because same-voxel collisions in a
big 3D grid within one dispatch are the *rare* case), the particle-splat
target here is a small 2D screen buffer that many-to-one accumulation is the
*entire point of* — texel collisions are the *common* case, especially for
a zoomed-out camera or a dense data cluster. A non-atomic load-add-store
translation would not be "the same kind of preserved bug", it would be a
**materially different, much lossier image** than upstream produces.

**Two options, one recommendation:**

| option | mechanism | fidelity | cost |
|---|---|---|---|
| **(a) CHOSEN** | `atomic<u32>` **storage buffer**, `width*height` elements, row-major (`y*width+x`); `cs_particles_transform` `atomicAdd()`s into it, `cs_particles_blit` reads it back as plain (non-atomic) `u32` | **exact** — true atomicity, order-independent sum, identical final per-pixel counts to D3D11 | resource *type* change (texture → buffer) that `main.cpp`/`graphics.h` have no host-side home for yet — see §8 |
| (b) rejected | non-atomic `texture_storage_2d<r32uint, read_write>`, load-add-store RMW (same shape as the M2 deposit-race quirk) | **not** a port — introduces a *new* race with a *much* higher, structurally different collision rate than the 3D case it superficially resembles | none (drop-in shape), but wrong |

(a) is implemented in `cs_particles_transform.wgsl` (binding at
`@group(1) @binding(0)`, line 157, `var<storage, read_write> tex_out:
array<atomic<u32>>`, `atomicAdd(&tex_out[out_idx], ...)` at lines 385 and
393) and consumed in `cs_particles_blit.wgsl` (`@group(1) @binding(0)`,
line 98, `var<storage, read> tex_in: array<u32>`, plain index at lines
162-164). See §8 for what this means for `display_tex_uint` on the host
side — **this is a real implementation task for M3, not just a shader-file
change.**

A **new required guard** falls out of choosing a buffer over a texture: WGSL
buffers use bounds-check-by-clamping semantics for an out-of-range dynamic
array index, whereas D3D11 typed-UAV texture writes are guaranteed
discarded/no-op when out of range (the M2b carryovers call this
"test-pinned" for the sim textures). Since `out_pos.x/.y` can legally land
exactly on `width`/`height` (the clip test at `cs_particles_transform.hlsl:91`
is `> 1.0`, not `>= 1.0`, so `out_posf.x == 1.0` — and therefore
`out_pos.x == width` — passes), an un-guarded buffer write would silently
corrupt the *last* buffer element with every off-right/bottom-edge splat,
every frame, instead of discarding them like D3D11 does. `cs_particles_
transform.wgsl:367-369` adds an explicit `if (out_pos.x >= width_px ||
out_pos.y >= height_px) { return; }` guard for this — **this guard has no
HLSL counterpart; it exists purely to restore the D3D11 discard-on-OOB-store
behaviour the buffer conversion would otherwise silently break.**

### 0.2 `cs_particles_blit`'s declared output type does not match its bound resource

`cs_particles_blit.hlsl:2` declares `RWTexture2D<float> tex_out` —
single-channel — but `main.cpp` creates the actual bound resource
(`display_tex`) as 4-channel `DXGI_FORMAT_R32G32B32A32_FLOAT` (pre-M2b
`main.cpp:577`; current fork `Format::RGBA32_FLOAT`,
`main.cpp:522`). This looks like a genuine upstream type/resource mismatch.
It is only *partially* recoverable by inspection: `ps_particles_color.hlsl:12`
(`float v = tex_trace.Sample(...)`, an implicit truncation to the first/red
component) confirms the *intent* — `val` is meant to land in the red
channel — but says nothing about what D3D11 actually wrote to G/B/A for a
mismatched-type UAV store, which is driver-defined and not something static
reading can settle. `cs_particles_blit.wgsl:187` writes
`vec4<f32>(val_out, 0.0, 0.0, 1.0)`, a reasonable but **unverified** default.
Currently inert (nothing downstream reads more than `.x`), so low priority
unless a future pass samples other channels of `display_tex`. Flagged again
in §7.

---

## 1. `RenderingConfig` uniform layout (byte offsets)

C++ `struct RenderingConfig` at `main.cpp:263-312`, `static_assert(sizeof ==
3*64 + 36*4)` at `main.cpp:313` (336 bytes total: 3 `mat4` + 36 scalars).
`cs_particles_transform.hlsl:7-32` and `cs_particles_blit.hlsl:4-29` each
declare a `cbuffer ConfigBuffer : register(b4)` with the **first 20** of the
36 scalars (plus all 3 matrices); the remaining 16 (`camera_x` onward —
path-tracer/PT-only fields) are absent from both HLSL cbuffers. Following
the M2 §2 rule ("declare the whole shared struct so one uniform buffer
serves every pass"), both WGSL drafts declare the **full** 336-byte struct.

Per the M2b carryover note (I3): `register(b4)` cbuffers move to
`@group(0) @binding(0)` in this fork, same as every other pass — this
applies to `cs_particles_transform`, `cs_particles_blit`, and (M4)
`cs_volpath`.

| # | member | type (C++) | type (WGSL) | offset | size | HLSL cbuffer line |
|---|---|---|---|---|---|---|
| — | `projection` | `Matrix4x4` | `mat4x4<f32>` | 0 | 64 | `:9` |
| — | `view` | `Matrix4x4` | `mat4x4<f32>` | 64 | 64 | `:10` |
| — | `model` | `Matrix4x4` | `mat4x4<f32>` | 128 | 64 | `:11` |
| 0 | `texcoord_map` | `int` | `i32` | 192 | 4 | `:12` |
| 1 | `trim_x_min` | `float` | `f32` | 196 | 4 | `:13` |
| 2 | `trim_x_max` | `float` | `f32` | 200 | 4 | `:14` |
| 3 | `trim_y_min` | `float` | `f32` | 204 | 4 | `:15` |
| 4 | `trim_y_max` | `float` | `f32` | 208 | 4 | `:16` |
| 5 | `trim_z_min` | `float` | `f32` | 212 | 4 | `:17` |
| 6 | `trim_z_max` | `float` | `f32` | 216 | 4 | `:18` |
| 7 | `trim_density` | `float` | `f32` | 220 | 4 | `:19` |
| 8 | `world_width` | `float` | `f32` | 224 | 4 | `:20` |
| 9 | `world_height` | `float` | `f32` | 228 | 4 | `:21` |
| 10 | `world_depth` | `float` | `f32` | 232 | 4 | `:22` |
| 11 | `screen_width` | `float` | `f32` | 236 | 4 | `:23` |
| 12 | `screen_height` | `float` | `f32` | 240 | 4 | `:24` |
| 13 | `sample_weight` | `float` | `f32` | 244 | 4 | `:25` |
| 14 | `optical_thickness` | `float` | `f32` | 248 | 4 | `:26` |
| 15 | `highlight_density` | `float` | `f32` | 252 | 4 | `:27` |
| 16 | `galaxy_weight` | `float` | `f32` | 256 | 4 | `:28` |
| 17 | `histogram_base` | `float` | `f32` | 260 | 4 | `:29` |
| 18 | `overdensity_threshold_low` | `float` | `f32` | 264 | 4 | `:30` |
| 19 | `overdensity_threshold_high` | `float` | `f32` | 268 | 4 | `:31` |
| 20 | `camera_x` | `float` | `f32` | 272 | 4 | not in cbuffer |
| 21 | `camera_y` | `float` | `f32` | 276 | 4 | not in cbuffer |
| 22 | `camera_z` | `float` | `f32` | 280 | 4 | not in cbuffer |
| 23 | `pt_iteration` | `int` | `i32` | 284 | 4 | not in cbuffer |
| 24 | `sigma_s` | `float` | `f32` | 288 | 4 | not in cbuffer |
| 25 | `sigma_a` | `float` | `f32` | 292 | 4 | not in cbuffer |
| 26 | `sigma_e` | `float` | `f32` | 296 | 4 | not in cbuffer |
| 27 | `trace_max` | `float` | `f32` | 300 | 4 | not in cbuffer |
| 28 | `camera_offset_x` | `float` | `f32` | 304 | 4 | not in cbuffer |
| 29 | `camera_offset_y` | `float` | `f32` | 308 | 4 | not in cbuffer |
| 30 | `exposure` | `float` | `f32` | 312 | 4 | not in cbuffer |
| 31 | `n_bounces` | `int` | `i32` | 316 | 4 | not in cbuffer |
| 32 | `ambient_trace` | `float` | `f32` | 320 | 4 | not in cbuffer |
| 33 | `compressive_accumulation` | `int` | `i32` | 324 | 4 | not in cbuffer |
| 34 | `guiding_strength` | `float` | `f32` | 328 | 4 | not in cbuffer |
| 35 | `scattering_anisotropy` | `float` | `f32` | 332 | 4 | not in cbuffer |

`sizeof == 336`, matching `main.cpp:313`'s `static_assert` exactly. Every
scalar member is 4-byte scalar/align, and the C++ struct already groups
them in fours (mirroring HLSL's float4-row cbuffer packing), so no member
straddles a 16-byte boundary and WGSL's default struct layout reproduces the
offsets with **zero fixups** — same conclusion as the M2 notes reached for
`SimulationConfig` (§2 there), now confirmed for `RenderingConfig` (which
that document left as future work, §2 there: "out of scope here, but... is
the one to check carefully").

**Matrix convention cross-check (also left open by M2, resolved here):**
`cpplib/maths.h:278-291`'s `Matrix4x4` stores **columns contiguously**
(`// NOTE: these are columns`, `v1..v4`) and defines `Matrix4x4::operator*
(Vector4 v)` as `result[i] = x[i]*v[0] + x[i+4]*v[1] + x[i+8]*v[2] +
x[i+12]*v[3]` — a standard column-major matrix-times-column-vector product.
HLSL's cbuffers default to column-major packing (no `row_major` keyword or
`/Zpr`/`D3DCOMPILE_PACK_MATRIX_ROW_MAJOR` used anywhere in this codebase),
and `mul(M, v)` for a matrix/vector pair computes the same column-vector
product. WGSL's `mat4x4<f32> * vec4<f32>` operator is defined identically
(column-major storage, `M*v` treats `v` as a column vector). **All three
layers agree — no transpose is needed anywhere in these shaders.**
`cs_particles_transform.wgsl` uses `cfg.view * in_posf` (line 307) and
`cfg.projection * world_pos` (line 310) directly as literal translations of
`mul(view_matrix, in_posf)` / `mul(projection_matrix, world_pos)`.

---

## 2. Display-tex sizing: the M2b-flagged open decision

`m2b-carryovers.md` #1 flags this explicitly: `display_tex`/
`display_tex_uint` are created at **logical** `window_width`/`window_height`
(`main.cpp:522-523`, marked `// M3: framebuffer-vs-logical decision`), while
`get_render_target_window()` and the swap-chain surface are configured at
**framebuffer** (physical) pixels. `cpplib/gpu/gpu_context.cpp:117-122` is
explicit that these differ on Retina: *"Use the framebuffer size, not the
logical window size: on Retina displays the framebuffer is 2x logical, and
configuring the surface [...] preserves the Retina fix"* — `ctx->width =
fb_w` comes from `glfwGetFramebufferSize`, not the logical size passed to
`glfwCreateWindow`. This is a real, measured 2x-per-axis (4x-area)
discrepancy on any Retina machine, not a hypothetical one.

### What each option changes, shader-by-shader

**Option A — keep logical sizing (current `main.cpp` state, unchanged).**
- `display_tex` / `display_tex_uint` stay at `window_width × window_height`.
- `rendering_config.screen_width/height` (`main.cpp:707-708`) already read
  `window_width/window_height` — **already consistent** with the texture
  size, so `cs_particles_transform`'s screen-space clip/splat math and
  `cs_particles_blit`'s dispatch shape need **zero shader changes**.
- The final textured quad (`vs_2d`/`ps_particles_color`) samples a
  logical-resolution texture but is rasterized into the **framebuffer**-
  resolution swap-chain render target — i.e. the sampler **upscales** by 2x
  per axis on Retina. `tex_sampler_display = graphics::get_texture_sampler()`
  (`main.cpp:529`) uses the default `Filter::POINT` (nearest-neighbour) —
  so the upscale is **blocky 2x pixelation**, not a smooth blur. This is a
  real, visible regression relative to the D3D11/Windows original, where
  "window size" was a single unambiguous concept and this distinction did
  not exist.
- Bug-for-bug purity: **highest** — literally nothing about the shaders or
  the uniform values needs to differ from a naive line-for-line port.

**Option B — switch to framebuffer sizing.**
- `display_tex` / `display_tex_uint` (and the new atomic accumulation
  buffer, §0.1) would need to be sized at the framebuffer's `width × height`
  (`RenderTarget.width/height` from `get_render_target_window()`, or
  equivalently `GpuContext.width/height`), not `window_width/window_height`.
- `rendering_config.screen_width/height` would need to be set from the
  *same* framebuffer dims, not `window_width/window_height` — **this is a
  `main.cpp` code change**, outside these shader drafts' scope, and it is
  load-bearing: `cs_particles_transform` uses `cfg.screen_width/height` both
  for the final pixel-space splat coordinate (`cs_particles_transform.wgsl`
  line 329) and (via this draft's new OOB guard, §0.1) for the bounds
  check, and `cs_particles_blit` uses it for the buffer row stride. If the
  *texture*/*buffer* were resized to framebuffer pixels but
  `screen_width/height` were left at logical values (or vice versa), the
  splat coordinates and the buffer's actual row stride would disagree —
  this is exactly the *"4x off"* failure mode the carryover note warns
  about, and it is a **silent** one (no validation error; the image just
  looks wrong, most likely as strided/torn garbage or partial coverage in
  one quadrant).
- No shader-*text* difference beyond what's already parameterized through
  `cfg.screen_width/height` and the texture/buffer dimensions passed in from
  the host at bind-group-creation time — the WGSL is agnostic to which
  convention is chosen, **provided** the host keeps texture size, buffer
  size, and `screen_width/height` mutually consistent.
- Visual result: crisp, full native-resolution particle rendering on
  Retina, matching what "restoring the visualization" should look like to a
  user in 2026 rather than reproducing an accidental Windows-era conflation
  of logical and physical pixels.
- Cost: 4x more texels/buffer elements for `display_tex`/`display_tex_uint`
  (trivial at typical window sizes — a few MB, nowhere near the VAC-scale
  volume textures' GB budget) and one `main.cpp` edit outside this task's
  scope.

### Recommendation: **Option B (framebuffer sizing)**

The upstream D3D11 code has no logical/physical distinction to be faithful
*to* — this is a genuinely new platform reality introduced by porting to
macOS/Retina, not a preserved-vs-fixed upstream-quirk question the "never
fix upstream math" rule was written for. Given M3's stated goal is
*restoring* the particle visualization (i.e., a user-facing feature, not a
correlation-sensitive simulation kernel like the M2 shaders), rendering it
at native/crisp resolution is the right target, and it is consistent with
how the rest of the render surface (`get_render_target_window`,
`depth_buffer`) already treats framebuffer pixels as ground truth. **This
requires a `main.cpp` change beyond these shader files** (texture/buffer
creation dims + the `rendering_config.screen_width/height` assignment) —
call this out explicitly as an M3 implementation task, not something to
silently assume already true. If the M3 plan prefers minimum-diff-first,
Option A is a safe, zero-shader-change fallback that can ship first and be
upgraded to Option B in a follow-up once the visual regression is confirmed
tolerable or not.

---

## 3. `cs_particles_transform` — semantic difference table

| # | area | HLSL | WGSL | difference bridged | risk |
|---|---|---|---|---|---|
| T1 | atomic accumulation target | `:1` `RWTexture2D<uint> tex_out: register(u0)`, `:99,102` `InterlockedAdd` | `:157` `var<storage, read_write> tex_out: array<atomic<u32>>`, `:385,393` `atomicAdd(&tex_out[...], ...)` | **THE HARD PROBLEM** — see §0.1. Resource type change texture→buffer; exact behavioural match, not an approximation, but requires a host-side (`main.cpp`) resource change | **H / blocker-class** |
| T2 | OOB store on the new buffer | none in HLSL (D3D11 typed-UAV OOB write = guaranteed discard) | `:367-369` explicit `if (out_pos.x >= width_px \|\| out_pos.y >= height_px) { return; }` | **new required guard**, not present in the HLSL — see §0.1. WGSL/WebGPU buffer OOB index semantics are clamp-based, not discard-based, and `out_pos` can legally equal `width`/`height` exactly (boundary is `> 1.0`, not `>=`, at HLSL:91) | **H** |
| T3 | cbuffer register move | `:7` `cbuffer ConfigBuffer : register(b4)` | `:148` `@group(0) @binding(0)` | per carryover I3 / M2b convention | M |
| T4 | struct subset → full struct | `:7-32` declares 20/36 scalars | `:93-148` declares all 36 | shared uniform buffer across the whole M3/M4 render chain, see §1 | L |
| T5 | matrix multiply order | `:85,88` `mul(view_matrix, in_posf)`, `mul(projection_matrix, world_pos)` | `:307,310` `cfg.view * in_posf`, `cfg.projection * world_pos` | verified NOT to need a transpose — see §1's matrix cross-check (this resolves the open question the M2 notes left at their §2) | M (resolved) |
| T6 | scalar-vector broadcast literal | `:81` `1.0.xxx` | `:294` `vec3<f32>(1.0)` | HLSL scalar-broadcast literal syntax has no WGSL equivalent; identical value | L |
| T7 | swizzle compound-assign | `:81-82` `in_posf.xyz = ...`, `in_posf.yz *= -1` | `:293-298` rewritten through a local `var p: vec3<f32>` and a full `vec4<f32>(...)` reconstruction | avoids relying on WGSL's swizzle-lvalue rules for a *multi-component, non-trivial* compound assignment (`*=` on a 2-component swizzle) — see §7 item 2 for why this was not risked as a direct port | L (as rewritten) / **uncertain if ported literally** |
| T8 | scalar-vector arithmetic broadcast | `:90` `out_posf * 0.5 + 0.5` | `:321` `out_posf * 0.5 + vec4<f32>(0.5)` | unlike T6/assignment-broadcast, mixed `vecN op scalar` for `+,-,*,/` IS a defined WGSL overload (contrast M2 notes A15, which was about *assignment*, not arithmetic) — the explicit `vec4<f32>(0.5)` here is optional, kept for readability | L |
| T9 | float→uint truncation | `:96` `uint2 out_pos = uint2(out_posf.xy)` | `:338` `vec2<u32>(screen_pos)` | same conversion-hazard class as M2 notes A6; gated to `[0, width]`/`[0, height]` **inclusive** by the `> 1.0` (not `>= 1.0`) clip test at HLSL:91 — this inclusivity is exactly why T2's guard is required | M |
| T10 | `RWStructuredBuffer` access mode | `:2-5` all `RWStructuredBuffer<float>` | `:174-177` narrowed to `var<storage, read>` | narrowing precedent from M2's `cs_field_decay` D1 (this shader only ever reads `particles_x/y/z/t`) | L |
| T11 | dead RNG code | `:34-61` `wang_hash`/`random`/`random_sphere`, never called from `main()` | `:189-217` ported verbatim, still unused | kept for 1:1 file correspondence; Tint will DCE. Do not "clean up" — see file header | L |
| T12 | implicit int→uint at `random()` call site | `:44-45` `wang_hash(seed)` where `seed` is `int` | `:206` `wang_hash(bitcast<u32>(seed))` | dead code (T11), so unverified against a real compiler; `bitcast` reproduces HLSL's bit-preserving implicit conversion | L (dead) |
| T13 | `numthreads` | `:63` `[numthreads(10,10,10)]` = 1000 invocations | `:236-240` `override WG_X/Y/Z: u32 = 10u`, `@workgroup_size(WG_X,WG_Y,WG_Z)` | **identical shape to `cs_agents_propagate`'s M2 blocker (A20)**, but ALREADY resolved for this build: `cpplib/gpu/gpu_context.cpp:53,64-65` requests the adapter's full `maxComputeInvocationsPerWorkgroup` (not the WebGPU default 256) and `cs_agents_propagate.wgsl` already runs at `WG(10,10,10)` in the shipped M2b build. **Proven safe, not merely hoped for.** | L (de-risked by M2b) |
| T14 | flat-index bijection | `:66` `idx = index + 1000*(group_id.x + group_id.y*10 + group_id.z*100)` | `:261-264` generalised via `@builtin(num_workgroups)`, identical technique to `cs_agents_propagate.wgsl` | reduces to the HLSL constants for the original dispatch `(10,10,grid_z)`; bijection under reshape | L |
| T15 | dispatch truncation | `main.cpp` (pre-M2b) `grid_z = (NUM_PARTICLES/100)/THREAD_GROUP_SIZE` | documented at `:259` (QUIRK comment) / `:261-264` (index math); no bounds guard added | same `QUIRK(dispatch_truncation)` class as M2 notes A30 — particles past `grid_z*100000` are never dispatched by this pass either | M |
| T16 | dead branch / no-splat case | `:97-100` `t<0.0 && galaxy_weight<=0.001` → neither `InterlockedAdd` fires | `:377-390` same, commented as `QUIRK(galaxy_weight_gate)` | preserved verbatim; plausibly an upstream oversight (falling through to the 10-weight branch might have been intended) but not "fixed" | M |
| T17 | HLSL `t` variable naming | `:71` local `float t = particles_t[idx]` | `:177` (binding), `:270` (read) | `particles_t` is bound from `particles_buffer_theta` at the original dispatch (`set_structured_buffer(&particles_buffer_theta, 6)`) — `t` is short for theta, not "type"/"time"; documented so a future reader doesn't misread the sign check at HLSL:97 | L |

---

## 4. `cs_particles_blit` — semantic difference table

| # | area | HLSL | WGSL | difference bridged | risk |
|---|---|---|---|---|---|
| B1 | accumulation source | `:1` `RWTexture2D<uint> tex_in: register(u0)` | `:98` `var<storage, read> tex_in: array<u32>` | same buffer as `cs_particles_transform`'s `tex_out` (§0.1), reinterpreted **non-atomic** here — see B2 for why that's safe | **H** (paired with T1) |
| B2 | cross-dispatch visibility without atomics | implicit D3D11 UAV hazard-tracking barrier between two `Dispatch()` calls on one command list | implicit WebGPU memory barrier between two compute passes on one command encoder | the two dispatches are sequential and non-overlapping (transform fully completes, then blit runs), so a plain (non-atomic) read here is memory-safe even though the *write* side needed atomicity — documented in the binding comment (`cs_particles_blit.wgsl:60-73`) | M (reasoning, not a literal HLSL line) |
| B3 | resource type mismatch | `:2` `RWTexture2D<float> tex_out` (1-channel) bound to a 4-channel `display_tex` resource | `:121` `texture_storage_2d<rgba32float, write>`, full `vec4<f32>` store (written at `:187`) | **UNCERTAIN** — see §0.2 and §7. `val` confirmed (via `ps_particles_color.hlsl:12`'s implicit truncation) to land in `.x`; G/B/A values are an unverified default `(0,0,1)` | **H** (uncertain, currently inert) |
| B4 | texture read → buffer index | `:34` `tex_in[dispatchThreadId.xy]` | `:162-164` `idx = coord.y * width_px + coord.x; tex_in[idx]` | row-major linearisation, MUST agree with `cs_particles_transform`'s write-side index math and with the actual dispatch width — a host-side (`main.cpp`) invariant, not shader-checkable | M |
| B5 | magic threshold `10000` | `:35` `if (val < 10000)` | `:178-181` same literal | `QUIRK(hardcoded_10000_threshold)` — not sourced from the uniform; coincides with `cs_particles_transform`'s galaxy-splat weight, so >1000 overlapping agent-splats on one pixel are misclassified as "a galaxy pixel" and skip `sample_weight` scaling. Kept verbatim | M |
| B6 | `numthreads(1,1,1)` | `:31` | `:146` `@workgroup_size(1,1,1)` | far under any workgroup-invocation limit — **no blocker**, unlike T13/M2's numthreads issues. Flagged as a pure-performance (not fidelity) reshape opportunity, left literal in this draft | L |
| B7 | dispatch shape | original `run_compute(window_width, window_height, 1)` | unchanged; `@builtin(global_invocation_id)` gives `(x,y,0)` directly, one workgroup per pixel | see §2 for which `width`/`height` this should actually be (window_width/height today; framebuffer recommended) | M (tied to §2) |
| B8 | dead entry params | `:32-33` `SV_GroupThreadID`, `SV_GroupID` declared, unused | `:147-152` dropped | same precedent as M2 notes D14 | L |

---

## 5. `vs_2d` + `ps_particles_color` — semantic difference table

| # | area | HLSL | WGSL | difference bridged | risk |
|---|---|---|---|---|---|
| V1 | two files → one module | `vs_2d.hlsl`, `ps_particles_color.hlsl` (separate `VertexShader`/`PixelShader` objects) | one `wgpu::ShaderModule`, `vs_main`/`fs_main` entry points | WGSL puts both stages in one file; no semantic difference, purely a file-organisation one | L |
| V2 | vertex layout | `vs_2d.hlsl:3-4` `POSITION` (float4), `TEXCOORD` (float2) | `:60-65` `@location(0) position: vec4<f32>`, `@location(1) texcoord: vec2<f32>` | matches `main.cpp:328-345`'s `quad_vertices` stride (6 floats = 24 B: 16 B position + 8 B texcoord) exactly; HLSL semantic *names* have no WGSL equivalent, replaced by explicit numbered locations | L |
| V3 | `SV_POSITION` / interstage varying | `vs_2d.hlsl:9-10` | `:67-77` `@builtin(position)` + `@location(0)` | standard, mechanical | L |
| V4 | passthrough body | `vs_2d.hlsl:13-21` | `:82-89` | no math; direct 1:1 | L |
| V5 | render bind-group convention (proposal) | `ps_particles_color.hlsl:7-8` `Texture2D tex_trace: register(t0)`, `SamplerState tex_sampler_trace: register(s0)` | `:103-104` `@group(1) @binding(0)` (texture), `@binding(1)` (sampler) | **new proposal** — `binding == 2*slot` for the texture, `2*slot+1` for its paired sampler, where `slot` is the argument to `graphics::set_texture`/`set_texture_sampler`. See §6 for the full rationale and the main.cpp call-site survey that constrains it | M (proposal, unvalidated by any existing binding code) |
| V6 | implicit vector truncation | `ps_particles_color.hlsl:12` `float v = tex_trace.Sample(...)` (float4→float) | `:118` `textureSample(...).x` | HLSL implicit narrowing (compiler-warned) made explicit; also the only static evidence for B3's channel-intent question | L |
| V7 | unclamped highlight curve | `:13-15` `1.0-exp(-0.0001*v)`, no upper clamp | `:126-130` | `QUIRK(unclamped_exp_highlight)` — naturally bounded in (0,1) for finite non-negative `v`, so inert; kept verbatim, not clamped | L |
| V8 | fixed teal ramp, no clamp | `:16-17` `v/3` scaling, no clamp | `:143-144` | `QUIRK(fixed_teal_ramp)` — `v/3 > 1.0` overshoots into out-of-range color, silently clipped by the swap-chain's implicit clamp-on-present. Render-stage analogue of the compute-stage OOB questions elsewhere in this port; harmless, flagged so it isn't mistaken for a bug in an M5 visual diff | L |
| V9 | no cbuffer in either shader | — | — | unlike `cs_particles_transform`/`cs_particles_blit`, neither `vs_2d.hlsl` nor `ps_particles_color.hlsl` declares a cbuffer, so this module needs **no** `@group(0)` binding at all — noted explicitly since every other shader in this port does | L |

---

## 6. Bind-group layout proposal

### Compute passes (`cs_particles_transform`, `cs_particles_blit`)

Unchanged convention from M2 (`docs/superpowers/research/m2/wgsl-drafts/
translation-notes.md` §6), confirmed still accurate against the *currently
shipped* `cpplib/graphics.cpp:604-657` implementation (`run_compute`):

```
@group(0) @binding(0)  var<uniform> cfg: RenderingConfig    // IF the shader declares one
@group(1) @binding(N)  <pass resource>, N == the HLSL register index
```

`graphics.cpp:597` detects `@group(0)` textually (`strstr(code, "@group(0)")`)
to decide whether to bind a uniform bind group at all — both M3 compute
shaders declare one (`ConfigBuffer`/`RenderingConfig`), so both need
`@group(0)`.

**`cs_particles_transform`**

| group | binding | resource | WebGPU type | HLSL register |
|---|---|---|---|---|
| 0 | 0 | `cfg` | `uniform`, min size 336 | `b4` |
| 1 | 0 | `tex_out` | storage buffer, `atomic<u32>` array, `read_write` | `u0` |
| 1 | 2 | `particles_x` | storage buffer, `f32` array, `read` | `u2` |
| 1 | 3 | `particles_y` | storage buffer, `f32` array, `read` | `u3` |
| 1 | 4 | `particles_z` | storage buffer, `f32` array, `read` | `u4` |
| 1 | 6 | `particles_t` (theta) | storage buffer, `f32` array, `read` | `u6` |

**`cs_particles_blit`**

| group | binding | resource | WebGPU type | HLSL register |
|---|---|---|---|---|
| 0 | 0 | `cfg` | `uniform`, min size 336 | `b4` |
| 1 | 0 | `tex_in` | storage buffer, **plain `u32`** array, `read` | `u0` (same physical buffer as transform's `tex_out`) |
| 1 | 1 | `tex_out` | storage texture, `rgba32float`, `write`, 2d | `u1` (== `display_tex`) |

### Render pass (`vs_2d` / `ps_particles_color`) — new proposal

`cpplib/graphics.h:116-119`'s render-stage API (`set_texture`,
`set_texture_sampler`) is currently a **stub** (`graphics.cpp:461-464`,
carryover #3: *"the moment M3 loads real shader sources these must become
real compiles [...] or `is_ready` gives false confidence"*), so there is no
existing binding convention to match against — this is a genuine proposal,
constrained only by the `slot` argument shape at the call sites
(`main.cpp:1147-1148` and the `VM_VOLUME*` block at `main.cpp:1157-1211`,
which pairs texture+sampler at the SAME slot number, e.g. slot 0 =
trace+trace-sampler, slot 1 = palette+palette-sampler).

Proposed convention, consistent with the compute-side "`binding == register
index`" rule but doubled to cover HLSL's separate `t`/`s` register spaces:

```
@group(0) @binding(0)      var<uniform> cfg: RenderingConfig   // IF present
@group(1) @binding(2*N)    texture bound via set_texture(_, N)
@group(1) @binding(2*N+1)  sampler bound via set_texture_sampler(_, N)
```

**`vs_2d_ps_particles_color`** (this file needs no `@group(0)` — see V9)

| group | binding | resource | WebGPU type | main.cpp call | HLSL register |
|---|---|---|---|---|---|
| 1 | 0 | `tex_trace` (== `display_tex`) | `texture_2d<f32>` | `set_texture(&display_tex, 0)` | `t0` |
| 1 | 1 | `tex_sampler_trace` (== `tex_sampler_display`) | `sampler` | `set_texture_sampler(&tex_sampler_display, 0)` | `s0` |

This proposal generalises cleanly to the `VM_VOLUME*` shaders (M4 scope):
slot 0 → bindings 0/1, slot 1 → bindings 2/3, matching
`main.cpp:1162-1163`/`:1171`/`:1179` exactly. Not implemented or validated
anywhere yet — flagged for whoever writes the M3 render-pipeline plumbing
(the `set_texture`/`set_texture_sampler` stub replacement carryover #3
calls for).

---

## 7. Uncertain lines

Ordered by how likely each is to bite.

### Would materially change behaviour if wrong

1. **`cs_particles_blit.wgsl:187` — G/B/A channel values on the
   `textureStore`.** See §0.2. `val_out` in `.x`/red is confirmed by
   `ps_particles_color.hlsl:12`'s implicit-truncation read; `(0.0, 0.0,
   1.0)` for G/B/A is an unverified default, not something recoverable from
   the HLSL alone (the type mismatch between the declared `RWTexture2D
   <float>` and the actual 4-channel resource is itself the open question).
   Currently inert. **Would need a real D3D11 capture (or accepting the
   ambiguity) to resolve with confidence.**
2. **`cs_particles_transform.wgsl:293-298` — the `in_posf.xyz`/`in_posf.yz`
   rewrite (T7).** HLSL:81-82 do `in_posf.xyz = (...)` then
   `in_posf.yz *= -1`. WGSL's swizzle-lvalue rules permit **non-repeating**
   swizzles as assignment targets (`.yz` does not repeat components, so a
   plain `in_posf.yz = ...` is legal), but I am not confident a **compound**
   assignment operator (`*=`) is accepted on a multi-component swizzle by
   the pinned Tint revision — I have seen this flagged as disallowed in
   some WGSL discussions and could not verify against a live compiler. The
   draft sidesteps the question entirely via a local `vec3<f32>` and a full
   `vec4<f32>(...)` reconstruction, which is unambiguously legal WGSL and
   numerically identical. If a reviewer wants the more literal
   `in_posf.yz *= vec2<f32>(-1.0);` form for closer HLSL correspondence,
   test it against Tint first.
3. **`cs_particles_transform.wgsl:206` / `:210-211` — `bitcast<u32>(seed)` /
   `bitcast<i32>(hash)` for the dead RNG functions (T11/T12).** Unreachable
   code (verified: nothing in `main()` calls `random_local`/`random_sphere`),
   so this is unverified against a real compiler and, being dead, cannot
   affect any observable behaviour either way. Low priority to resolve.
4. **`cs_particles_blit.wgsl` buffer/dispatch-width agreement (B4/B7).** The
   row-major index math (`coord.y * width_px + coord.x`) requires
   `cfg.screen_width` to equal the ACTUAL dispatch width AND the actual
   buffer row stride at bind-group-creation time. This is a host-side
   (`main.cpp`) invariant with no shader-side assertion possible (WGSL has
   no way to read the dispatch shape it was invoked with, beyond
   `num_workgroups`, which for `numthreads(1,1,1)` does equal the pixel
   dimensions — so a `@builtin(num_workgroups)` cross-check IS technically
   possible here, unlike in the propagate/decay shaders. Not added in this
   draft; worth considering if M3 wants a belt-and-suspenders assert.)

### Would compile but is a judgment call, not a bug

5. **§2's sizing decision (Option A vs B).** Not a WGSL-syntax question at
   all — a host-architecture one. Recommendation given (Option B,
   framebuffer sizing) but explicitly flagged as needing a `main.cpp` change
   outside this task's file scope.
6. **§6's render bind-group proposal (`binding == 2*slot [+1]`).** Genuinely
   new convention, unvalidated against any working code (unlike the compute
   convention, which M2b's `cpplib/graphics.cpp:604-657` already implements
   and exercises). Whoever de-stubs `set_texture`/`set_texture_sampler`
   (carryover #3) should treat this as a starting proposal, not a settled
   contract.
7. **Reserved-word / mechanical sweep.** `random_local` was renamed from
   HLSL's `random` because `random` collides with nothing in WGSL's
   reserved-word list that I could find, but was renamed anyway to avoid
   any future collision with a WGSL builtin naming convention (there is no
   builtin literally named `random`, unlike M2's `mod` situation) — purely
   defensive, not a required rename. `wang_hash`, `random_sphere`, `main`,
   `vs_main`, `fs_main`, `tex_trace`, `tex_sampler_trace`, `tex_out`,
   `tex_in`, `cfg` all checked against the WGSL reserved-word list by
   inspection; none collide.

### Open questions for the M3 plan

- **Does the atomic-buffer conversion (§0.1) get its own `graphics::`
  API, or is it special-cased in `main.cpp`?** `graphics::clear_texture_uint`
  (used by the original `ClearUnorderedAccessViewUint(display_tex_uint.
  ua_view, ...)` call, carryover #2) clears a `Texture2D`; once
  `display_tex_uint`'s accumulation role moves to a raw storage buffer,
  something needs to clear THAT buffer to zero once per frame instead (or
  in addition — `display_tex_uint` as an actual `R32_UINT` *texture* could
  still exist solely so the existing texture-clear kernel keeps working, if
  the M3 implementation chooses to keep a texture around for clearing
  purposes only and copy/alias into the buffer — not recommended, adds a
  copy pass with no upstream analogue; simplest is a new
  `graphics::clear_storage_buffer_uint`-shaped entry point). This is a
  `cpplib/graphics.{h,cpp}` + `main.cpp` task, not a shader-file task, and
  is the single largest piece of "M3 implementation impact" this research
  surfaced.
- **`display_tex_uint`'s `Texture2D` struct entry in `main.cpp:523` becomes
  dead** once the accumulation target is a buffer (T1) — `main.cpp:1633`'s
  `graphics::release(&display_tex_uint)` and the type's continued existence
  need re-examination in the M3 implementation plan; this draft does not
  assume it still exists as a texture at all.
- **Should `cs_particles_blit`'s `numthreads(1,1,1)` be reshaped for
  performance (B6)?** Flagged as a safe, quirk-free optimisation
  opportunity, not attempted in this draft to keep it maximally literal for
  review.
