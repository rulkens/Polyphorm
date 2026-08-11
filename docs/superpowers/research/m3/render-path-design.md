# M3 render-path design: WebGPU draw_mesh, shaders, pipelines

Research-only design doc for M3 (particle rendering). No code changed. Target
API surface: `cpplib/graphics.h` (39 functions, unchanged) / `cpplib/graphics.cpp`
(current stub bodies). Reads the M2b-landed state on `macos-webgpu-port`
(commit `b78ccd6`).

**Bottom line up front:** no `graphics.h` *function signature* changes are
required for M3. One additive, backward-compatible struct-field change is
recommended (§1). Everything else is internal to `graphics.cpp` (new shadow
state + a lazy pipeline cache) or a main.cpp call-site fix that M4, not M3,
needs to make (§5, §8 risk #2).

---

## 0. Scope boundary (YAGNI)

M3, per the task brief, is the **quad-blit path**: `graphics::draw_mesh` made
real, `vertex_shader_2d` (`vs_2d.hlsl`) + `pixel_shader_2d`
(`ps_particles_color.hlsl`) compiled for real, bound to `quad_mesh`
(`main.cpp:671`, stride 24 B), sampling `display_tex`. `ui::` stays on
`ui_stub.cpp` until M4 (confirmed: `cpplib/ui_stub.cpp` is the active TU, not
`cpplib/ui.cpp`), so the histogram/UI draws at `main.cpp:1301-1407` and the
panel at `1425+` are out of scope — they never call `graphics::draw_mesh`.

This document designs the **render-path machinery** in `graphics.cpp`
(shader compile, pipeline cache, vertex layout, pass structure, bind
convention) generally enough that M4's volume path (`vertex_shader`/`vs_3d.hlsl`,
`super_quad_mesh`, stride 28 B, `BlendType::ALPHA`, up to 5 pixel shaders) is
a drop-in consumer of the same machinery, not a redesign. Volume-specific
correctness (alpha formula, texcoord_map matrices) is **not** verified here —
flagged as an M4 follow-up in the risk register.

Not in scope, and deliberately not designed here: `ui::` real implementation,
`cs_volpath` compute (M4), offscreen `RenderTarget`s (never created by
main.cpp — inventory §3), depth testing (§7).

**Companion compute work M3 also needs** (not a `graphics.h` render-surface
concern, noted so the milestone isn't accidentally declared done from this
doc alone): `cs_particles_transform.hlsl`/`cs_particles_blit.hlsl` still need
WGSL ports and dispatch call sites in the `VM_PARTICLES` block
(`main.cpp:940-959` shows the sibling pattern for `cs_agents_propagate`,
nothing equivalent exists yet for particle splat/blit), and the
`ClearUnorderedAccessViewUint` replacement `graphics::clear_texture_uint(&display_tex_uint, 0)`
per `docs/superpowers/research/m2/m2b-carryovers.md` item 2 — `display_tex_uint`
is allocated (`main.cpp:523`) but nothing clears it yet (`grep clear_texture`
in main.cpp shows only the two `trail_tex`/`trace_tex` F2/F8 handlers). Without
these three additions, `draw_mesh(&quad_mesh)` will render a real pipeline
sampling a `display_tex` that compute never wrote — visually blank/undefined,
not a render-path bug.

---

## 1. Shader compilation

### The two-entry-point-in-one-module question

main.cpp loads vertex and pixel shaders from **separate files** today and
will keep doing so in WGSL — `vs_2d.hlsl`/`ps_particles_color.hlsl`,
`vs_3d.hlsl`/`ps_volume_*.hlsl` (5 pixel-shader variants sharing one vertex
shader), `vs_2d.hlsl`/`ps_volpath.hlsl` (M4). **Design: keep them separate.**
Each `.wgsl` file compiles to its own `wgpu::ShaderModule` with exactly one
entry point named `main`, tagged `@vertex` or `@fragment` as appropriate —
mirroring the compute shaders' convention exactly (`cs_*.wgsl` all use
`fn main`). `wgpu::RenderPipelineDescriptor` natively accepts two distinct
`wgpu::ShaderModule`s (`vertex.module` and `fragment.module`) — this is
standard WebGPU usage, not a workaround, so **no call-site change** is
needed: `get_vertex_shader_from_code`/`get_pixel_shader_from_code` keep
compiling exactly the one file handed to them, exactly as
`main.cpp`'s (not-yet-written) M3 loader will call them, matching the
`load_compute` lambda pattern already in `main.cpp:482-490`.

Rejected: combining vs+ps into one module with two entry points. It would
require restructuring the shader source layout (pairing files, or
duplicating shared structs across a merged file) for zero benefit — Dawn
does not require co-located entry points to share a pipeline. Pure added
complexity; YAGNI.

### Compilation body (mirrors `get_compute_shader_from_code`, `graphics.cpp:552-599`)

```
VertexShader get_vertex_shader_from_code(char *code, uint32_t code_length) {
    device.PushErrorScope(Validation);
    wgpu::ShaderModule module = device.CreateShaderModule(wgsl-source desc);
    // pop scope, wait_for(&done), same pattern as compute — valid = !had_error
    VertexShader vs;
    vs.module = module;
    vs.valid = !had_error;
    vs.uses_group0 = strstr(code, "@group(0)") != NULL;   // same trick as ComputeShader
    return vs;
}
```
`get_pixel_shader_from_code` is the mirror image. This replaces the current
stub (`graphics.cpp:541-550`) that sets `valid=true` unconditionally — the
exact hazard `m2b-carryovers.md` item 3 calls out ("`is_ready` gives false
confidence").

Note the limit of `CreateShaderModule`-time validation: it only proves the
WGSL parses and type-checks in isolation. It does **not** prove the shader
is compatible with a specific vertex-buffer layout, blend target format, or
its paired stage — those errors surface only when `CreateRenderPipeline` runs,
which is necessarily later (§2), because stride/blend/format aren't known
until `draw_mesh` time. `is_ready(&vertex_shader)` after load therefore means
"this WGSL module compiled," not "this shader is fully wired" — document this
distinction inline in graphics.h/.cpp so it isn't mistaken for the stronger
compute-shader guarantee `is_ready(&compute_shader)` gives (compute pipelines
*are* fully built at `get_compute_shader_from_code` time, since compute has
no stride/blend/format axes).

### `graphics.h` change

Additive struct fields only, mirroring `ComputeShader::uses_group0` (already
declared, `graphics.h:82`):

```cpp
struct VertexShader { wgpu::ShaderModule module; bool valid = false; bool uses_group0 = false; };
struct PixelShader  { wgpu::ShaderModule module; bool valid = false; bool uses_group0 = false; };
```
No function signatures change. Every existing call site (`is_ready(&vs)`,
`release(&vs)`, `set_vertex_shader(&vs)`) keeps compiling unmodified.

---

## 2. Pipeline management: lazy cache keyed at draw_mesh time

`wgpu::RenderPipeline` is immutable once created and needs vertex-buffer
layout + blend + target format + both shader modules simultaneously — but
`graphics.h`'s D3D11-shaped API sets vertex shader, pixel shader, and blend
state independently (`set_vertex_shader`, `set_pixel_shader`,
`set_blend_state`) and doesn't learn the vertex stride until `draw_mesh(Mesh*)`
is called. This is the same shape problem the compute path already solved
differently (one shader = one pipeline, built eagerly at
`get_compute_shader_from_code` time) — render can't do that because the
pipeline depends on *three* independently-set pieces of shadow state plus
the mesh argument.

**New shadow state** (`graphics.cpp`, alongside the existing `g_blend`):

```cpp
static VertexShader *g_vertex_shader = nullptr;
static PixelShader  *g_pixel_shader  = nullptr;
static RenderTarget  g_render_target;          // set by set_render_targets_viewport
static BoundSlot g_render_slots[MAX_SLOTS];     // textures/samplers, see §5
```

`set_vertex_shader`/`set_pixel_shader` become `g_vertex_shader = shader;` /
`g_pixel_shader = shader;` (currently `(void)shader;` stubs, `graphics.cpp:601-602`).
`set_render_targets_viewport` stops being a pure no-op — see §4.

**Cache key**: `{vs->module.Get(), ps->module.Get(), g_blend, mesh->vertex_stride}`.
Target format and depth-attachment presence are **constant** for the whole
M3/M4 lifetime (always the window's sRGB BGRA8 view, per §4/§7 — main.cpp
never creates or draws to an offscreen `RenderTarget`, confirmed by
`graphics-api-inventory.md` §3: `get_render_target(w,h,format)` is unused),
so they're deliberately **left out of the key** rather than threaded through
as extra dimensions — documented as a scope-limiting simplification in
`graphics.cpp` with a comment pointing at this file, so a future milestone
that adds an offscreen target or a depth-tested pipeline knows to widen the
key rather than silently reuse a wrong-format cached pipeline.

**Cache storage**: a flat `std::vector` of `{key fields..., wgpu::RenderPipeline}`,
scanned linearly in `draw_mesh`. The whole program has at most ~2 vertex
shaders × ~7 pixel shaders × 2 blend states × 2 strides ≈ well under 20 live
entries — a hash map buys nothing here and `wgpu::ShaderModule` doesn't have
an ergonomic hash; linear scan over `.Get()` pointer equality is simpler and
fast enough at this N. Cache entries hold a `wgpu::RenderPipeline` (refcounted);
`graphics::release()` (`graphics.cpp:134-147`) needs a one-line addition
(`g_pipeline_cache.clear();`) alongside the existing `g_clear3d = {}` etc.
resets, or pipelines silently outlive the device teardown.

**`draw_mesh(Mesh *mesh)` body, in order:**

1. Assert `g_vertex_shader && g_vertex_shader->valid && g_pixel_shader && g_pixel_shader->valid`
   (mirrors `run_compute`'s `assert(g_compute_shader && g_compute_shader->valid)`,
   `graphics.cpp:609`).
2. Look up/build the `wgpu::RenderPipeline` per the key above. On build: vertex
   buffer layout from the stride table (§3); `fragment.targets[0].format =
   BGRA8UnormSrgb` (the same format `window_view()` hardcodes at
   `graphics.cpp:93`); blend from `g_blend` (§2b below); `primitive.topology`
   from `mesh->topology`; `depthStencil = nullptr` (§7); wrapped in the same
   `PushErrorScope`/`PopErrorScope`/`wait_for` pattern as
   `get_compute_shader_from_code` — a pipeline-creation validation failure is
   a loud `fatal()`, not a silent no-op (extends the M2b-carryover principle
   from shader compile to pipeline build — see risk #7).
3. Build bind group 0 (uniform) **only if** `g_vertex_shader->uses_group0 ||
   g_pixel_shader->uses_group0` — reusing the exact `g_uniform_buffer`/
   `g_uniform_size` globals `set_constant_buffer` already populates
   (`graphics.cpp:211-217`), the same way `run_compute` conditionally binds
   group 0 today (`graphics.cpp:618-629`). `fatal()` with the same message
   shape if group 0 is needed but no uniform is bound.
4. Build bind group 1 from `g_render_slots[]` (§5).
5. Open a render pass, `LoadOp::Load` (§4), `SetPipeline`, `SetBindGroup(0,...)`
   if present, `SetBindGroup(1,...)`, `SetVertexBuffer(0, mesh->vertex_buffer)`,
   `Draw(mesh->vertex_count)`, `End()`.

**Blend mapping** (`BlendType` → `wgpu::BlendState`), from the D3D11
original's recorded behaviour (`graphics-api-inventory.md` §"Blend/rasterizer
state": OPAQUE = zeroed blend desc i.e. disabled, ALPHA = src-alpha/inv-src-alpha):

- `OPAQUE`: `ColorTargetState.blend = nullptr` (blending disabled, straight overwrite).
- `ALPHA`: color = `{srcFactor: SrcAlpha, dstFactor: OneMinusSrcAlpha, operation: Add}`,
  alpha = the same formula (not verified against the D3D11 alpha-channel op
  specifically — inventory only confirms the color-channel formula). Low risk
  for M3: `ps_particles_color.hlsl` always writes `.a = 1.0` (`shaders/ps_particles_color.hlsl:14,17`),
  so the alpha-channel blend formula is unobservable in M3's only draw. M4's
  volume slabs vary per-fragment alpha and must be screenshot-verified against
  upstream — see risk #6.

`main.cpp:532` calls `set_blend_state(ALPHA)` once at startup and never
toggles it — every M3/M4 draw is alpha-blended, matching upstream exactly
(inventory: "every draw call for the rest of the program runs with alpha
blending on").

---

## 3. Vertex layouts: explicit table, not inference

Exactly two vertex shapes exist in the whole program (`graphics-api-inventory.md`
§"Mesh/drawing"): `quad_mesh` (`main.cpp:671`, stride `sizeof(float)*6` = 24 B:
`float4 position` + `float2 texcoord`, matching `vs_2d.hlsl:1-5`) and
`super_quad_mesh` (`main.cpp:670`, stride `sizeof(float)*7` = 28 B: `float4
position` + `float3 texcoord`, matching `vs_3d.hlsl:1-5`). Both are non-indexed
triangle lists (`get_mesh`'s `assert(indices == nullptr...)`, `graphics.cpp:484`,
already enforces this).

**Design: an explicit `switch (mesh->vertex_stride)` table in `draw_mesh`,
not a generic format-inference or shader-source parser:**

| `vertex_stride` | attribute 0 | attribute 1 |
|---|---|---|
| 24 | `float32x4` @ offset 0, `@location(0)` | `float32x2` @ offset 16, `@location(1)` |
| 28 | `float32x4` @ offset 0, `@location(0)` | `float32x3` @ offset 16, `@location(1)` |
| other | `fatal("draw_mesh: no vertex layout for stride N")` | |

Explicitly **not** reviving the D3D11 original's `get_vertex_input_desc_from_shader`
text tokenizer — the inventory (§3/§"Shaders" row) flags it as already having
a live bug ("hardcodes vertex_input_count=2 instead of using the parsed
count") and it's the kind of parser complexity YAGNI says to avoid: two
strides, two fixed layouts, a `fatal()` if a third ever shows up. Every WGSL
vertex shader's input struct must use `@location(0)` for position and
`@location(1)` for texcoord to match — a one-line comment convention, not
enforced in code (matching the fork's existing trust level for WGSL/C++
struct-layout agreement elsewhere, e.g. `SimulationConfig`/`RenderingConfig`).

---

## 4. Render pass structure

**Current state**: `clear_render_target` (`graphics.cpp:109-123`) opens a
`BeginRenderPass` with `LoadOp::Clear` and immediately `End()`s it — a
complete, standalone pass that touches the render target once and closes.
`set_render_targets_viewport` is a documented no-op
(`graphics.cpp:101-107`: "Nothing to record until a clear or (M3) a draw").

**Design**: `set_render_targets_viewport` stops being a no-op — it records
`g_render_target = *buffer` (a value copy; `RenderTarget` is a small POD with
no owned resources beyond a `wgpu::Texture`/`wgpu::TextureView` the caller
still owns). This is the only way `draw_mesh(Mesh*)` — which takes no target
argument — can know where to render, since `graphics.h`'s draw call doesn't
carry one (matching D3D11's implicit-current-render-target model, which is
exactly what `OMSetRenderTargets` gave the original). `main.cpp:1137` already
calls `set_render_targets_viewport(&render_target_window)` immediately before
`clear_render_target` every frame, so this shadow state is always populated
before the mode-dispatch `draw_mesh` calls that follow it.

`draw_mesh` opens its **own** standalone render pass, `LoadOp::Load` (preserve
the prior clear or draw), targeting `g_render_target.is_window ?
window_view() : g_render_target.rt_view`, draws once, `End()`s — the same
"one self-contained encoder-recording call per API call" shape
`clear_render_target`, `run_clear`, and `run_compute` already use. **Rejected**:
keeping one render pass open across multiple `draw_mesh`/`clear_render_target`
calls spanning separate `graphics::` function invocations — it would need new
"is a pass currently open" state and a matching explicit close call that
doesn't exist in `graphics.h` today, for a perf win that doesn't matter at
this call volume (M3: exactly one `draw_mesh` per frame; M4 volume: exactly
one of three mutually-exclusive `draw_mesh` calls per frame, `main.cpp:1194/1200/1206`).
Simplicity over elegance, per house rules.

`window_view()` is safe to call twice in one frame (once from
`clear_render_target`, once from `draw_mesh`) — it's guarded by
`g_surface_tex_acquired` (`graphics.cpp:82-99`) and only acquires the surface
texture once per frame regardless of call count; each call just builds a
fresh (cheap) `wgpu::TextureView` over the same underlying texture.

**Viewport**: no explicit `pass.SetViewport(...)` call needed. WebGPU's
default viewport spans the full color-attachment extent, and the attachment
is always `render_target_window` at framebuffer resolution — exactly the
"full window" viewport upstream always used (inventory §4: "main.cpp never
sets a partial/custom viewport"). Leave `set_render_targets_viewport` without
a `SetViewport` call; the comment at `graphics.cpp:103-105` about "viewport is
full-target by default" was already correct and needs no further work.

**Compute interleaving**: none for M3. The task brief's mention of upstream's
histogram-drawing-between-draws doesn't apply — `ui::` is the stub, so no
`graphics::` calls happen between `clear_render_target` and the mode-dispatch
`draw_mesh`. M4's `VM_PATH_TRACING` branch does interleave a compute dispatch
(`run_compute` for `cs_volpath`, `main.cpp:1218-1244`) before its own
`draw_mesh(&quad_mesh)` at `main.cpp:1250` — that's already legal under this
design: `run_compute` and `draw_mesh` both call `ensure_encoder()`/record
into whatever `g_encoder` currently is and don't require exclusive access to
it, so a compute pass and a render pass can be recorded back-to-back into the
same command encoder before a single `flush_commands()` — no new plumbing
needed for that case either.

---

## 5. Bind convention: group 0 uniform, group 1 textures/samplers

**Group 0** — one binding, `@binding(0)`, `var<uniform>`. Reuses the exact
`g_uniform_buffer`/`g_uniform_size` shadow slot `set_constant_buffer` already
populates for compute (`graphics.cpp:211-217`, `set_constant_buffer` already
asserts `slot == 0` and documents "group 0 binding 0"). Built **conditionally**,
gated on `vs->uses_group0 || ps->uses_group0` (§1, §2) — mirroring
`run_compute`'s `uses_group0` gate exactly. Stage visibility is **not** set
manually: `wgpu::RenderPipeline::GetBindGroupLayout(0)` auto-derives it by
inspecting which of the vertex/fragment stages actually declared `@group(0)`
in their compiled module (same auto-layout mechanism `ComputePipeline::GetBindGroupLayout`
already relies on) — no new code needed to decide "vertex-only" vs
"fragment-only" vs "both."

For M3 this group is **never built** — `vs_2d.hlsl`/`ps_particles_color.hlsl`
declare no cbuffer/uniform at all (confirmed by reading both files: `vs_2d.hlsl`
is a pure passthrough, `ps_particles_color.hlsl` only declares `Texture2D`/`SamplerState`).
`main.cpp`'s M3 quad-draw call site therefore correctly never calls
`set_constant_buffer` before `draw_mesh(&quad_mesh)` in the `VM_PARTICLES`
branch — matching the current code exactly (`main.cpp:1143-1150` only calls
`update_constant_buffer`, never `set_constant_buffer`, for that draw).

**Group 1** — textures and samplers by register index, `set_texture(tex, slot)`
/ `set_texture_sampler(sampler, slot)`. **Design decision, and the one place
this deliberately diverges from the compute path's convention:**

```
texture at slot N  -> @group(1) @binding(2*N)
sampler at slot N  -> @group(1) @binding(2*N + 1)
```

Why not `binding == slot` directly (the compute convention, `run_compute`
binds `g_compute_slots[i]` at literal `binding = i`,
`graphics.cpp:632-649`)? Because **every** render call site pairs a texture
and its sampler at the *same* slot number — `set_texture(&display_tex, 0)` +
`set_texture_sampler(&tex_sampler_display, 0)` (`main.cpp:1147-1148`),
`set_texture(&trace_tex, 0)` + `set_texture_sampler(&tex_sampler_trace, 0)`
(`main.cpp:1157-1159`), `set_texture(&palette_trace_tex, 1)` +
`set_texture_sampler(&tex_sampler_color_palette, 1)` (`main.cpp:1162-1163`),
and so on through every M4 volume variant. A literal `binding = slot` scheme
would put the texture and its sampler at the *same* WGSL binding number in
the *same* group — a hard Dawn validation error (`texture_2d` and `sampler`
can't share a binding). The `2N`/`2N+1` split is deterministic, collision-free,
and needs zero new state beyond the existing per-slot shadow array (just
reuse the `BoundSlot`-shaped struct already defined for compute,
duplicated as `g_render_slots[MAX_SLOTS]` since render and compute bindings
are logically independent — main.cpp does bind compute and render textures
concurrently within one frame, e.g. `VM_PATH_TRACING`'s compute dispatch at
slot 0-4 followed immediately by a render-stage bind at slot 0
(`main.cpp:1222` vs `1248`), which must not collide).

`set_texture(Texture2D*, slot)`/`set_texture(Texture3D*, slot)` write
`{kind: SAMPLED_TEX, view: t->sr_view}` into `g_render_slots[slot]`.
`set_texture_sampler` writes `{kind: SAMPLER, sampler: s->sampler}` into the
**same** `g_render_slots[slot]` struct but a **different field** than the
texture write would use — i.e. `BoundSlot` needs to carry both a possible
`view` and a possible `sampler` for the same slot simultaneously (today's
compute `BoundSlot` is a tagged union / one-`kind`-at-a-time design, which
doesn't fit here since a render slot is a *pair*). Cleanest fix: change
`g_render_slots[slot]` to a small `{wgpu::TextureView view; wgpu::Sampler
sampler;}` struct (no `kind` tag needed — presence is `view != nullptr` /
`sampler != nullptr`), and `draw_mesh`'s bind-group-1 builder emits an entry
at `2*slot` for every slot with a non-null `view` and an entry at `2*slot+1`
for every slot with a non-null `sampler`. `unset_texture(slot)` clears
`.view` only (leaving `.sampler` — matches `main.cpp`'s actual unset pattern,
e.g. `unset_texture(0)` at `main.cpp:1150` after a draw, samplers are never
explicitly unset anywhere in main.cpp, they're just left bound and
overwritten next frame).

**Carryover flag for M4/compute**: `cs_volpath` (M4, not yet ported) will hit
the *identical* texture+sampler-at-the-same-slot collision on the
**compute** side — `set_texture_sampled_compute(&trace_tex, 1)` +
`set_texture_sampler_compute(&tex_sampler_trace, 1)` both target slot 1
(`main.cpp:1223-1224`), and today's `g_compute_slots[]` is a single-`kind`
tagged union that would let the second call silently clobber the first
(`graphics.cpp:466-472`: each `set_texture_*_compute`/`set_texture_sampler_*_compute`
call does `g_compute_slots[slot] = {}` then sets one `kind` — a same-slot
sampler call after a sampled-texture call zeroes the texture binding). This
is **latent today** only because no compute shader shipped so far pairs a
sampled texture with a sampler (`cs_agents_propagate`/`cs_field_decay`/
`cs_density_histo` are all storage-only). It will need the same fix
`draw_mesh` gets here (split kind into a texture-or-buffer slot **plus** an
independent sampler slot, or the same `2N`/`2N+1` binding-number split
applied to `run_compute`'s bind-group-1 builder) — flagged for whoever plans
M4's compute work, not this M3 doc's problem to fix.

---

## 6. Display sizing: logical offscreen, framebuffer-native swapchain

Per `m2b-carryovers.md` item 1: `display_tex`/`display_tex_uint` are sized
`window_width`/`window_height` (`main.cpp:522-523`) — **logical** pixels,
since `window_width` is the config-file `SCREEN_X`/`SCREEN_Y` value
(`main.cpp:369-382`) passed straight into `glfwCreateWindow` (`platform.cpp:46`,
which takes logical/point units) and never updated from the actual
framebuffer size afterward. `render_target_window`/`get_render_target_window`
(`graphics.cpp:73-79`) reports `g_gpu.width`/`height`, which **are**
framebuffer pixels — `gpu_context.cpp:117-123` explicitly queries
`glfwGetFramebufferSize` for the Retina-correct 2x value and configures the
surface at that resolution.

**Recommendation: keep the split. Offscreen simulation-side textures
(`display_tex`, `display_tex_uint`, and by extension `trail_tex_*`/`trace_tex`,
which are sized off the simulation grid, not the window, and are unaffected)
stay at logical resolution; only the final swapchain surface is
framebuffer-native.** Concretely:

- `display_tex`/`display_tex_uint` creation (`main.cpp:522-523`) stays
  unchanged — logical `window_width`/`window_height`.
- `cs_particles_transform`'s splat writes into `display_tex_uint` at logical
  coordinates (its WGSL port keeps `screen_width`/`screen_height` from
  `RenderingConfig` = `window_width`/`window_height`, `main.cpp:707-708`, also
  logical — self-consistent).
- `cs_particles_blit`'s dispatch stays `(window_width, window_height, 1)`
  (inventory §5 row 6: "one thread group dispatched per pixel," no `/8` or
  `/10` divisor) — no dispatch-dimension change needed, because both sides
  of the blit are already logical-sized.
- The final `draw_mesh(&quad_mesh)` renders into `render_target_window` at
  **framebuffer** resolution (2x on Retina) via the default full-attachment
  viewport (§4) — the rasterizer stretches the fullscreen NDC quad to cover
  the physical window regardless of `display_tex`'s native resolution;
  `tex_sampler_display` is `Filter::POINT` (`main.cpp:529`, `get_texture_sampler()`
  defaults), so the visible result is a nearest-neighbor 2x upscale of a
  logical-resolution render on Retina displays.

This is a **known, accepted softness for M3**, not a bug: it picks the
*simpler* half of the "pick one convention" ask (carryover item 1's actual
failure mode — 4x-off coverage — only happens if the two sides of the split
disagree, which this design prevents by keeping every simulation-side
dimension self-consistently logical). The alternative (resize `display_tex`
to framebuffer resolution for crisp Retina output) is strictly more code:
`cs_particles_transform`/`blit` dispatch dims and `RenderingConfig.screen_width/height`
would all need to move to framebuffer units, doubling `display_tex_uint`'s
atomic-splat texture area (4x pixel count at 2x/2x) for a resolution upgrade
that doesn't change correctness — explicitly deferred, not scoped into M3.
Revisit only if the 2x softness is visually unacceptable in practice.

---

## 7. Depth: out of scope for M3 (and M4's volume draw)

`depth_buffer` is created (`main.cpp:475`, `graphics::get_depth_buffer`) and
`is_ready`-asserted, but the D3D11 original never bound it either — inventory
§4 confirms: "even the [depth-buffer-in-scope] call site only calls the
single-arg [`set_render_targets_viewport`] overload... the depth buffer is
created but never actually bound." Depth-buffer-shaped effects (volume
slicing) are achieved entirely through draw order + alpha blending, not a
depth test, both upstream and in this fork's ported main.cpp (single-arg
`set_render_targets_viewport(RenderTarget*)` is the *only* overload that
exists in this fork's `graphics.h` — the two-arg `(RenderTarget*, DepthBuffer*)`
overload was already dropped, so there's no live call path that could bind it).

**Recommendation**: `draw_mesh`'s render pass never sets a
`depthStencilAttachment`, and the pipeline cache key (§2) never includes a
depth format/test axis — for the entire M3/M4 lifetime as currently planned.
`DepthBuffer`'s `ds_view` stays real-but-inert, exactly as `graphics.h:42`'s
comment already documents ("created by main.cpp but never bound"). This
costs nothing extra: no plumbing to add, no plumbing to remove later if a
depth-tested feature ever does show up (M5+ or beyond) — it would be a
strictly additive change to the pipeline cache key and the pass descriptor,
not a rework.

---

## 8. Risk register

Ordered most to least likely to actually break something.

1. **Texture/sampler same-slot binding collision** (§5). If `draw_mesh`'s
   group-1 builder is implemented with `binding == slot` instead of the
   `2N`/`2N+1` split, the very first M3 draw (`display_tex@0` +
   `tex_sampler_display@0`) fails Dawn bind-group-layout validation
   immediately. High likelihood of being the first bug hit precisely because
   it's the *first* draw call exercised. Mitigation: the split is specified
   above; test with exactly this pair before anything else.

2. **Missing `set_constant_buffer(&rendering_settings_buffer, 0)` before M4's
   volume `draw_mesh` calls.** `vs_3d.hlsl` declares a cbuffer at (WGSL)
   `@group(0)` and needs the uniform bound, but `main.cpp`'s `VM_VOLUME*`
   branch (`main.cpp:1194, 1200, 1206`) only calls `update_constant_buffer`,
   never `set_constant_buffer`, for `rendering_settings_buffer` — the only
   existing call site for that buffer is the unrelated compute dispatch at
   `main.cpp:1221` (`VM_PATH_TRACING`). Under this design, the first volume
   draw after M3 lands will hit the `uses_group0` gate's `fatal()` ("shader
   declares uniform but none is bound") unless M4 adds the missing call
   immediately before each `draw_mesh(&super_quad_mesh)`. Not an M3 blocker
   (`vs_2d`/`ps_particles_color` use no uniform at all — §5), but flagged
   loudly here so M4 doesn't have to rediscover it by crashing.

3. **Display-resolution "fix" that breaks the logical/framebuffer split**
   (§6). Someone resizing `display_tex` to framebuffer resolution without
   simultaneously updating `cs_particles_blit`'s dispatch dims and
   `RenderingConfig.screen_width/height` reproduces exactly the 4x-coverage
   bug `m2b-carryovers.md` item 1 warned about. Mitigation: this doc's §6
   keeps all three in lockstep at logical resolution; any future change must
   touch all three together, not just the texture size.

4. **Pipeline-creation validation errors surfacing only at first draw, not at
   shader load** (§1, §2). A vertex-shader/mesh-stride mismatch (e.g. wiring
   `vs_3d`'s 28-byte-stride input struct to `quad_mesh`'s 24-byte buffer)
   passes `is_ready` at load time and only fails inside `draw_mesh`'s
   `CreateRenderPipeline` error scope. Mitigation: `draw_mesh` must `fatal()`
   loudly on a pipeline-creation error (never silently no-op), per the
   already-adopted M2b principle of preferring "loud and named" failures
   over silent wrong output.

5. **Compute-side texture+sampler collision, carried to M4** (§5 carryover
   note). `cs_volpath`'s slot-1 sampled-texture-plus-sampler pair will hit
   the same class of bug `run_compute`'s single-`kind` `BoundSlot` design has
   today, once M4 ports it. Not an M3 code change, but the M3 design's fix
   pattern (independent view/sampler storage per slot) is the template to
   reuse — noted so it isn't rediscovered from scratch.

6. **`BlendType::ALPHA`'s alpha-channel blend formula is a guess** (§2). The
   inventory only confirms the color-channel `SrcAlpha`/`OneMinusSrcAlpha`
   formula from the D3D11 desc; the alpha-channel op wasn't independently
   recorded. Unobservable in M3 (`ps_particles_color` always outputs
   `a = 1.0`). M4's volume pixel shaders vary alpha per-fragment and blend
   correctness must be screenshot-compared against upstream before trusting
   the guess.

7. **`release()` leaking cached `wgpu::RenderPipeline`s** (§2). If the new
   pipeline-cache vector isn't added to `graphics::release()`'s reset list
   (`graphics.cpp:134-147`, alongside `g_clear3d = {}` etc.), cached
   pipelines outlive the device teardown. Low severity (process is exiting
   anyway) but easy to forget and easy to fix — one line.

8. **Stale `warn_once("draw_mesh")`** (§2). Trivial: the current stub body
   (`graphics.cpp:499`) must be fully replaced, not left as a dead branch
   that could mask a real early-return bug during development.
