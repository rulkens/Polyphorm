# M3 — Particle Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The VM_PARTICLES visualization renders on macOS — millions of agent splats accumulated on the GPU and drawn to the window — with an automated headless pixels-exist test and a human visual check.

**Architecture:** The render path in `graphics.cpp` becomes real (vs/ps compilation, lazy pipeline cache, `draw_mesh`), following `docs/superpowers/research/m3/render-path-design.md` (referenced below as **DESIGN**). The particle compute chain (`cs_particles_transform` → `cs_particles_blit`) lands from the drafts in `docs/superpowers/research/m3/wgsl-drafts/` (notes referenced as **NOTES**), with the D3D11 atomic-texture accumulation replaced by an `atomic<u32>` StructuredBuffer (NOTES §0.1, exact-fidelity choice). main.cpp's VM_PARTICLES block is rewired accordingly.

**Tech Stack:** C++17, Dawn WebGPU (pinned), WGSL, CMake/ctest.

## Global Constraints

- Permanent fork; no #ifdefs; quirks preserved bug-for-bug (never fix upstream math; `QUIRK(...)` comments per house style).
- Binding conventions (load-bearing):
  - Compute: `@group(0) @binding(0)` uniform; `@group(1) @binding(N)` = slot N = HLSL register index (unchanged from M2b).
  - Render (NEW, from DESIGN §5 + NOTES §6, independently converged): `@group(0) @binding(0)` uniform; `@group(1) @binding(2*N)` = texture at slot N, `@group(1) @binding(2*N+1)` = sampler at slot N. Render shadow slots are a `{view, sampler}` PAIR per slot, independent from compute slots.
- Every dispatch/draw site: set ITS uniform at slot 0 (if its shaders declare `@group(0)`), set exactly the declared slots, unset after.
- **Display sizing decision (adjudicated): Option A — logical sizing stays** (NOTES §2 Option A; DESIGN §6). `display_tex`, the new accumulation buffer, and `rendering_config.screen_width/height` all stay at logical `window_width/height`; the swapchain stays framebuffer-native; result is a 2× nearest-neighbor upscale on Retina, accepted for M3. Any future framebuffer upgrade must change ALL THREE coupled sites together (creation dims, screen_width/height, blit dispatch) — never one alone.
- Build incrementally ONLY: `cmake -B build`, `nice -n 19 cmake --build build -j 8`. NEVER wipe `build/`, NEVER bare `-j`.
- All ctest suites green before every commit; never weaken assertions. Existing 6 suites must stay green throughout (energy_smoke especially — the sim must not regress).
- After ANY manual `./build/polyphorm` run: verify the process exited; kill it if not. Leave NO instances running.
- Fatal errors name their cause. Pipeline-creation failures are loud `fatal()`s, never silent no-ops (DESIGN risk #4).
- Reference docs are canonical for rationale: DESIGN (`docs/superpowers/research/m3/render-path-design.md`), NOTES (`docs/superpowers/research/m3/wgsl-drafts/translation-notes.md`), carryovers (`docs/superpowers/research/m2/m2b-carryovers.md`).
- End every commit message with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

## File Structure

- `cpplib/graphics.h` — additive: `VertexShader`/`PixelShader` gain `wgpu::ShaderModule module; bool uses_group0;` fields; new decl `void clear_structured_buffer(StructuredBuffer *buffer);`
- `cpplib/graphics.cpp` — Task 1: real vs/ps compile, render shadow state, pipeline cache, `draw_mesh`, `set_texture`/`set_texture_sampler`/`unset_texture`, `set_render_targets_viewport` records target, `clear_structured_buffer`, `release()` additions
- `shaders/vs_2d.wgsl`, `shaders/ps_particles_color.wgsl` — Task 2 (split from the combined draft)
- `shaders/cs_particles_transform.wgsl`, `shaders/cs_particles_blit.wgsl` — Task 2 (from drafts)
- `tests/render_path_tests.cpp` — Task 1 (headless render-machinery tests)
- `tests/shader_compile_tests.cpp` — Task 2 (extend with the four new shaders)
- `main.cpp` — Task 3 (VM_PARTICLES wiring, accumulation buffer, vs/ps loading)
- `CMakeLists.txt` — Tasks 1, 2

---

### Task 1: Render machinery in graphics.cpp + headless tests

**Files:**
- Modify: `cpplib/graphics.h`, `cpplib/graphics.cpp`
- Create: `tests/render_path_tests.cpp`
- Modify: `CMakeLists.txt` (new test target)

**Interfaces:**
- Consumes: DESIGN §§1-5, 7 — implement its decisions exactly; it cites every graphics.cpp line it builds on.
- Produces (Task 3 relies on these semantics): real `get_vertex_shader_from_code`/`get_pixel_shader_from_code` (error-scope validated, `valid`, `uses_group0`); `set_vertex_shader`/`set_pixel_shader` record shadow pointers; `set_texture(Texture2D*/Texture3D*, slot)` + `set_texture_sampler(sampler, slot)` populate paired `g_render_slots[slot] = {view, sampler}`; `unset_texture(slot)` clears `.view` only; `set_render_targets_viewport(RenderTarget*)` records `g_render_target`; `draw_mesh(Mesh*)` builds/caches the pipeline and draws in its own `LoadOp::Load` pass; `clear_structured_buffer(StructuredBuffer*)` zero-fills via `encoder.ClearBuffer`.

- [ ] **Step 1: graphics.h amendments (additive only)**

```cpp
struct VertexShader { wgpu::ShaderModule module; bool valid = false; bool uses_group0 = false; };
struct PixelShader  { wgpu::ShaderModule module; bool valid = false; bool uses_group0 = false; };
```
(Replace the existing empty-struct definitions; every call site — `is_ready`, `release`, `set_*_shader` — keeps compiling.) Next to `clear_texture_uint`, add:
```cpp
void clear_structured_buffer(StructuredBuffer *buffer);   // zero-fills; GPU-side ClearBuffer
```

- [ ] **Step 2: Shader compilation (DESIGN §1)**

Implement `get_vertex_shader_from_code`/`get_pixel_shader_from_code` mirroring `get_compute_shader_from_code`'s error-scope pattern exactly (PushErrorScope → CreateShaderModule → PopErrorScope callback → `wait_for` → `valid = !had_error`), plus `uses_group0 = strstr(code, "@group(0)") != NULL`. Each compiles ONE file with ONE entry point named `main` (`@vertex` or `@fragment`). Update `release(VertexShader*)`/`release(PixelShader*)` and `is_ready` if their current stub bodies assume no module field. Document inline (per DESIGN §1): render `is_ready` means "module compiled", weaker than compute's "pipeline built" — stride/blend/format errors surface at draw_mesh.

- [ ] **Step 3: Render shadow state + setters (DESIGN §2, §5)**

```cpp
static VertexShader *g_vertex_shader = nullptr;
static PixelShader  *g_pixel_shader  = nullptr;
static RenderTarget  g_render_target = {};
struct RenderSlot { wgpu::TextureView view; wgpu::Sampler sampler; };
static RenderSlot g_render_slots[MAX_SLOTS];
```
`set_vertex_shader`/`set_pixel_shader` record pointers. `set_texture(Texture2D*, slot)` / `set_texture(Texture3D*, slot)` write `.view` (assert slot < MAX_SLOTS); `set_texture_sampler` writes `.sampler`; `unset_texture` clears `.view` only (samplers are never unset by main.cpp — DESIGN §5). `set_render_targets_viewport(RenderTarget *buffer)` records `g_render_target = *buffer`. No SetViewport call (DESIGN §4: default full-attachment viewport is correct).

- [ ] **Step 4: draw_mesh + lazy pipeline cache (DESIGN §2, §3, §4)**

Replace the warn-once stub entirely (DESIGN risk #8). Body, in order:
1. `assert(g_vertex_shader && g_vertex_shader->valid && g_pixel_shader && g_pixel_shader->valid);`
2. Pipeline cache: flat `static std::vector` of `{WGPUShaderModule vs, ps; BlendType blend; uint32_t stride; wgpu::RenderPipeline pipeline}`, linear scan on `.Get()` pointer equality. On miss, create: vertex layout from the stride table — 24 B → `float32x4@0 loc0` + `float32x2@16 loc1`; 28 B → `float32x4@0 loc0` + `float32x3@16 loc1`; other → `fatal("draw_mesh: no vertex layout for stride %u")`. `fragment.targets[0].format = BGRA8UnormSrgb` (same as `window_view()`); blend: OPAQUE → `blend = nullptr`, ALPHA → color `{SrcAlpha, OneMinusSrcAlpha, Add}`, alpha same formula (DESIGN §2's documented guess; unobservable in M3 since ps_particles_color writes a=1.0); `depthStencil = nullptr` (DESIGN §7); topology triangle-list; wrap creation in the error-scope pattern and `fatal()` on validation error naming the shaders. Comment the cache-key scope limitation (format/depth constant — DESIGN §2).
3. Bind group 0 iff `g_vertex_shader->uses_group0 || g_pixel_shader->uses_group0`, from `g_uniform_buffer`/`g_uniform_size`, `fatal()` if needed-but-unbound (same message shape as run_compute). Stage visibility comes free from `pipeline.GetBindGroupLayout(0)` auto-layout.
4. Bind group 1 from `g_render_slots[]`: entry at `2*slot` per non-null `.view`, at `2*slot+1` per non-null `.sampler`, layout from `pipeline.GetBindGroupLayout(1)`.
5. `ensure_encoder()`; render pass with `LoadOp::Load`, `StoreOp::Store`, target `g_render_target.is_window ? window_view() : g_render_target.rt_view` (match the field names `clear_render_target` uses); SetPipeline, SetBindGroup(0) if built, SetBindGroup(1), `SetVertexBuffer(0, mesh->vertex_buffer)`, `Draw(mesh->vertex_count)`, End.

- [ ] **Step 5: clear_structured_buffer + release() additions**

```cpp
void clear_structured_buffer(StructuredBuffer *buffer)
{
    ensure_encoder();
    g_encoder.ClearBuffer(buffer->buffer, 0, buffer->size);
}
```
(Adapt field names to the actual StructuredBuffer struct.) In `graphics::release()` add `g_pipeline_cache.clear(); g_vertex_shader = nullptr; g_pixel_shader = nullptr; g_render_target = {}; for slots: g_render_slots[i] = {};` (DESIGN risk #7).

- [ ] **Step 6: Headless render tests**

`tests/render_path_tests.cpp`, same init pattern as `tests/shader_compile_tests.cpp` (`graphics::init()`/`release()`). Tests (write minimal inline WGSL in the test file — these are test shaders, not ports):

Test 1 — vs/ps compile validation: a trivial `@vertex fn main` module and `@fragment fn main` module compile (`is_ready` true, `uses_group0` false); an invalid WGSL string returns `valid=false` without crashing.

Test 2 — offscreen draw + readback (the pixels-exist harness, kills DESIGN risks #1 and #4 headlessly): implement `graphics::get_render_target(uint32_t width, uint32_t height, Format format)` for real if it is still a stub — create a texture with `RenderAttachment|CopySrc` usage + view, `RGBA32_FLOAT` supported, populate the RenderTarget struct with `is_window=false` (match existing struct fields) — this makes the whole render path headless-testable and is a legitimate part of the 39-function surface. Then:
- create a 4×4 offscreen RT; `set_render_targets_viewport`; `clear_render_target(rt, 0,0,0,1)`;
- create an 8×8 R32_FLOAT Texture2D, `clear_texture(&tex, 0.5f)` — sampled source;
- vertex shader: fullscreen quad passthrough matching the 24 B layout; fragment shader: `textureSample(tex, samp, uv).x` into rgba — bind via `set_texture(&tex, 0)` + `set_texture_sampler(&samp, 0)` (exercises the 2N/2N+1 convention exactly as main.cpp will);
- `Mesh quad = get_mesh(<the 6-vertex 24 B quad from main.cpp's quad_vertices layout>, 6, 24, NULL, 0, 0);` `set_blend_state(ALPHA);` `draw_mesh(&quad);` `flush_commands()` (or the capture path's implicit flush);
- read back: `encoder.CopyTextureToBuffer` into a MapRead buffer (bytesPerRow padded to 256 — 4×4 RGBA32F rows are 64 B, pad to 256), blocking map via `wait_for`, assert every texel ≈ (0.5, …, 1.0 alpha per the fragment's write). Write this small readback helper inside the test file.

Test 3 — draw with a shader pair that declares `@group(0)` but no uniform bound: this must `fatal()`/abort — verify by documenting it as a death-behavior comment instead of running it in-process (no death-test harness exists; a comment + the covered positive path in Test 2 is acceptable). Skip implementing an actual death test.

- [ ] **Step 7: CMake target + run**

Add `render_path_tests` copying the `shader_compile_tests` pattern (same sources, `-UNDEBUG`). Run `nice -n 19 cmake --build build -j 8 && ctest --test-dir build` — all 7 suites green.

- [ ] **Step 8: Commit**

```bash
git add cpplib/graphics.h cpplib/graphics.cpp tests/render_path_tests.cpp CMakeLists.txt
git commit -m "graphics: real render path — vs/ps compile, pipeline cache, draw_mesh, paired texture/sampler slots"
```

---

### Task 2: Particle-chain WGSL shaders land + compile validation

**Files:**
- Create: `shaders/cs_particles_transform.wgsl`, `shaders/cs_particles_blit.wgsl` (copy from `docs/superpowers/research/m3/wgsl-drafts/`, verbatim)
- Create: `shaders/vs_2d.wgsl`, `shaders/ps_particles_color.wgsl` (SPLIT from `docs/superpowers/research/m3/wgsl-drafts/vs_2d_ps_particles_color.wgsl`)
- Modify: `tests/shader_compile_tests.cpp`, `CMakeLists.txt` if needed

**Interfaces:**
- Consumes: Task 1's `get_vertex_shader_from_code`/`get_pixel_shader_from_code`.
- Produces: the four production shaders Task 3 loads. Bind contracts (NOTES §6): transform g0=RenderingConfig(336B), g1={0: atomic<u32> accum buffer, 2,3,4: particles x/y/z read, 6: particles_theta read}; blit g0=RenderingConfig, g1={0: plain u32 accum buffer read, 1: rgba32float storage texture write}; vs_2d/ps_particles_color: no group 0; ps has texture@`@group(1)@binding(0)` + sampler@`@binding(1)`.

- [ ] **Step 1: Copy the two compute drafts verbatim**

```bash
cp docs/superpowers/research/m3/wgsl-drafts/cs_particles_transform.wgsl shaders/
cp docs/superpowers/research/m3/wgsl-drafts/cs_particles_blit.wgsl shaders/
```
Edits only via the same escape-hatch discipline as M2b Task 3 (compile-error-driven, minimal, documented; NOTES §7 lists the uncertain lines — notably the T7 swizzle rewrite is already draft-side-safe and the T11/T12 dead RNG may need mechanical fixes). Any edit beyond a compile fix must cite the HLSL line and be flagged.

- [ ] **Step 2: Split the render draft into two files**

From `docs/superpowers/research/m3/wgsl-drafts/vs_2d_ps_particles_color.wgsl` create:
- `shaders/vs_2d.wgsl`: the vertex-stage structs + `@vertex fn main(...)` (rename from `vs_main`). Keep `@location(0) position: vec4<f32>`, `@location(1) texcoord: vec2<f32>` inputs; output `@builtin(position)` + `@location(0) texcoord` varying.
- `shaders/ps_particles_color.wgsl`: the fragment-stage bindings (`@group(1) @binding(0)` texture_2d<f32>, `@binding(1)` sampler) + `@fragment fn main(...)` (rename from `fs_main`), taking `@location(0) texcoord: vec2<f32>`, returning `@location(0) vec4<f32>`.
Duplicate any shared struct into both files as needed (separate modules can't share source). The varying location (0) must match across the two files — that is the inter-module contract. Preserve all QUIRK comments (V7 unclamped exp highlight, V8 fixed teal ramp) in the fragment file.

- [ ] **Step 3: Extend shader_compile_tests**

Add the two compute shaders via the existing `check_compiles` (expect `uses_group0=1` for both). Add a `check_compiles_vs`/`check_compiles_ps` pair using `get_vertex_shader_from_code`/`get_pixel_shader_from_code` for the two render files (expect `uses_group0=0` for both — V9). Run; iterate on compile errors per the escape hatches.

- [ ] **Step 4: Run full suite + commit**

`ctest --test-dir build` — all 7 suites green (the four new shaders compile).

```bash
git add shaders/cs_particles_transform.wgsl shaders/cs_particles_blit.wgsl shaders/vs_2d.wgsl shaders/ps_particles_color.wgsl tests/shader_compile_tests.cpp
git commit -m "shaders: land M3 particle chain (transform, blit, vs_2d, ps_particles_color)"
```

---

### Task 3: main.cpp VM_PARTICLES wiring + pixels-exist test + visual check

**Files:**
- Modify: `main.cpp`
- Modify: `tests/` (extend `render_path_tests.cpp` or a new `tests/particle_chain_tests.cpp` — implementer's choice, report it)
- Modify: `CMakeLists.txt` if a new test target

**Interfaces:**
- Consumes: everything above.
- Produces: a windowed run that shows moving particle splats; a headless ctest proving the compute chain writes nonzero pixels into display_tex.

- [ ] **Step 1: Resource changes in main.cpp**

- DELETE `display_tex_uint` entirely: creation (`Texture2D display_tex_uint = ...`), its `graphics::release(&display_tex_uint)`, and any comment referring to it (NOTES §7 open question 2 — the texture is dead once the accumulation target is a buffer).
- CREATE the accumulation buffer next to the other structured buffers:
```cpp
    // Particle splat accumulation (replaces upstream's R32_UINT UAV texture:
    // WGSL has no atomic storage textures — atomic<u32> buffer, row-major
    // y*width+x. See docs/superpowers/research/m3/wgsl-drafts/translation-notes.md §0.1.
    StructuredBuffer display_accum_buffer = graphics::get_structured_buffer(sizeof(uint32_t), window_width * window_height);
```
- Add `graphics::release(&display_accum_buffer);` to the cleanup block.

- [ ] **Step 2: Shader loading**

In the shader-loading region, load the four real shaders (replacing the `= {}` stubs for these four only; volpath/sort/volume-ps stubs stay):
```cpp
    ComputeShader draw_compute_shader_particle = load_compute(SHADER_ROOT "/cs_particles_transform.wgsl");
    ComputeShader blit_compute_shader = load_compute(SHADER_ROOT "/cs_particles_blit.wgsl");
```
and a `load_vs`/`load_ps` lambda pair mirroring `load_compute` (read file → `get_vertex_shader_from_code`/`get_pixel_shader_from_code` → assert `is_ready` → release file) for:
```cpp
    VertexShader vertex_shader_2d = load_vs(SHADER_ROOT "/vs_2d.wgsl");
    PixelShader pixel_shader_2d = load_ps(SHADER_ROOT "/ps_particles_color.wgsl");
```
(Other vs/ps variables keep their `= {}` M4 stubs.)

- [ ] **Step 3: The VM_PARTICLES block**

Restore the compute chain with full bind discipline (this replaces the "M3:" placeholder comment; consult the pre-M2b upstream shape in git history `git show 9c0b3b6:main.cpp` around line 1165 for the original ordering — set/dispatch/unset, but rewritten for the fork conventions):

```cpp
            if(vis_mode == VisualizationMode::VM_PARTICLES) {
                graphics::clear_structured_buffer(&display_accum_buffer);
                graphics::update_constant_buffer(&rendering_settings_buffer, &rendering_config);

                // Splat particles into the accumulation buffer.
                graphics::set_compute_shader(&draw_compute_shader_particle);
                graphics::set_constant_buffer(&rendering_settings_buffer, 0);  // per-dispatch, fork convention (carryover I3)
                graphics::set_structured_buffer(&display_accum_buffer, 0);
                graphics::set_structured_buffer(&particles_buffer_x, 2);
                graphics::set_structured_buffer(&particles_buffer_y, 3);
                graphics::set_structured_buffer(&particles_buffer_z, 4);
                graphics::set_structured_buffer(&particles_buffer_theta, 6);
                int32_t grid_z = (NUM_PARTICLES / 100) / THREAD_GROUP_SIZE;
                graphics::run_compute(10, 10, grid_z);
                graphics::unset_structured_buffer(0);
                graphics::unset_structured_buffer(2);
                graphics::unset_structured_buffer(3);
                graphics::unset_structured_buffer(4);
                graphics::unset_structured_buffer(6);

                // Blit accumulated counts into display_tex.
                graphics::set_compute_shader(&blit_compute_shader);
                graphics::set_constant_buffer(&rendering_settings_buffer, 0);
                graphics::set_structured_buffer(&display_accum_buffer, 0);
                graphics::set_texture_compute(&display_tex, 1);
                graphics::run_compute(window_width, window_height, 1);
                graphics::unset_structured_buffer(0);
                graphics::unset_texture_compute(1);

                // Draw display_tex to the window.
                graphics::set_vertex_shader(&vertex_shader_2d);
                graphics::set_pixel_shader(&pixel_shader_2d);
                graphics::set_texture(&display_tex, 0);
                graphics::set_texture_sampler(&tex_sampler_display, 0);
                graphics::draw_mesh(&quad_mesh);
                graphics::unset_texture(0);
            }
```
Preserve upstream's original ordering quirk if it differs (check `git show 9c0b3b6:main.cpp`: upstream set theta at slot 6 BEFORE the display texture at 0 — the order of set calls doesn't matter under shadow-state binding, keep the tidy order above). NOTE: `set_constant_buffer(&rendering_settings_buffer, 0)` here binds RENDER config at slot 0 for these dispatches; the next frame's top-of-loop `set_constant_buffer(&config_buffer, 0)` restores the sim uniform — verify that line is still unconditional.

- [ ] **Step 4: Headless pixels-exist test**

Extend the test suite (implementer's structural choice) with a headless test that mirrors the VM_PARTICLES chain: init device headless; build tiny particle buffers (e.g. 100 particles clustered mid-screen, positions in grid space with a RenderingConfig whose matrices are identity-ish orthographic — copy the projection math from main.cpp or use a simple centered ortho; document what you chose); run transform then blit exactly as Step 3 does (64×64 "screen"); copy `display_tex`'s 64×64 RGBA32F content to a MapRead buffer (reuse Task 1's readback helper); assert (a) at least one texel has `.x > 0` and (b) the OOB guard held — e.g. a particle crafted to land at exactly x==width splats nowhere (last-element corruption check per NOTES §0.1: last buffer element unchanged). All existing suites must still pass, especially energy_smoke.

- [ ] **Step 5: Full suite + windowed visual smoke**

`ctest --test-dir build` — all suites green. Then generate a dataset (`build/gen_test_dataset <scratch>`) and run windowed from `<scratch>`: `<repo>/build/polyphorm --dataset <scratch>/testdata` for ~15 seconds. Expected: the window shows a dark field with bright accumulating particle structure (three blobs converging into filaments), title bar shows pass counter. Take note of what you see, kill the process, report. If the window is blank: debug with the pixels-exist test as the bisect anchor (chain works headless → the draw path or sizing is at fault; DESIGN risk register is the checklist, risk #1 first).

- [ ] **Step 6: Commit**

```bash
git add main.cpp tests/ CMakeLists.txt
git commit -m "render: VM_PARTICLES live — atomic splat buffer, blit, textured quad draw"
```

---

### Task 4 (human gate): visual verification

After Task 3's final review, the coordinator asks the human to run:
```bash
build/gen_test_dataset /tmp/polyviz && cd /tmp/polyviz && <repo>/build/polyphorm --dataset /tmp/polyviz/testdata
```
and confirm: particles visible, structure emerges over seconds, F2 resets, Esc quits. This is the M1-style human gate; automated coverage cannot judge "looks like the real thing."

---

## Deferred / explicitly out of scope

- Framebuffer-native (crisp Retina) display sizing — documented upgrade (NOTES §2 Option B): change creation dims + `screen_width/height` + blit dispatch TOGETHER. Revisit after M3 ships if the 2× softness bothers.
- `cs_agents_sort` port (still default-off), volume shaders + volpath + ImGui (M4), export/save_texture3D (M4/M5 — still the standing M5-critical carryover).
- The compute-side texture+sampler same-slot collision (DESIGN §5 carryover): bites cs_volpath in M4; the RenderSlot pair pattern is the template. M4's plan must address `run_compute`'s BoundSlot.
- Blend alpha-channel formula verification vs upstream (DESIGN risk #6) — observable only in M4's volume rendering; screenshot-compare then.
- `cs_particles_blit`'s G/B/A channel values (NOTES §0.2) — unverified default `(0,0,1)`, currently inert; resolve only if something ever reads beyond `.x`.
- `numthreads(1,1,1)` blit reshape (NOTES B6) — pure perf, only if profiling demands.
