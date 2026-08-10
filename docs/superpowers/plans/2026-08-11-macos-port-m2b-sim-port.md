# M2b — Simulation Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** main.cpp compiles and runs on macOS with the MCPM simulation executing on the GPU, verified headless by a rising energy metric and by decay-boundary micro-tests that pin the port's bug-for-bug quirks.

**Architecture:** The two hand-translated WGSL drafts (agents propagate + field decay) move into `shaders/` and are joined by a new `cs_density_histo.wgsl` port. `main.cpp` is ported onto the M2a `graphics::` layer with full per-dispatch bind discipline, a stub `ui::`, and a `--headless N` mode that runs the sim without a window and self-checks that energy rises. Small graphics-layer amendments (group-0 hardening, `unset_structured_buffer`) land first — the M2a header freeze is over.

**Tech Stack:** C++17, Dawn WebGPU (pinned v20260807.193620), WGSL, CMake, ctest.

## Global Constraints

- Permanent fork: Windows-only code is deleted outright, never `#ifdef`'d.
- Bug-for-bug fidelity: quirks are preserved verbatim and toggleable via pipeline `override` constants. NEVER "fix" simulation math (truncated PI `3.141592`, RNG seed-guard typo, decay weight bug, non-periodic low boundary, dithered trace decay). Do not refactor RNG-consuming branches to `select()` — it desyncs the RNG stream.
- Binding convention (load-bearing): `@group(0) @binding(0)` = the single uniform config for the dispatch; `@group(1) @binding(N)` = slot-N resource where N is the HLSL register index. Bound group-1 slots must EXACTLY match the shader's declarations.
- Every dispatch site follows the discipline: `set_constant_buffer` for ITS uniform at slot 0, set exactly the group-1 slots its shader declares, dispatch, unset every group-1 slot it set. No reliance on leftover bindings (D3D11 habit).
- Build incrementally ONLY: `cmake -B build` to reconfigure, `nice -n 19 cmake --build build -j 8`. NEVER wipe `build/` or clean-configure into a fresh dir (re-downloads ~1.4GB of Dawn). NEVER bare `-j`.
- All tests run headless (`gpu::init_device`, no window) via `ctest --test-dir build`. Never commit failing tests; never weaken assertions to pass.
- Fatal errors name the specific feature/limit/file that failed.
- Granted limits on the target machine (M1 Max, verified in M2a): maxComputeInvocationsPerWorkgroup=1024 — the original `numthreads(10,10,10)`=1000 and `(8,8,8)`=512 fit unchanged, NO workgroup reshape. maxStorageBufferBindingSize = maxBufferSize = 4095 MiB.
- `SimulationConfig` is 16 four-byte scalars = 64 B, byte-identical across C++/WGSL. Never add `vec3` or arrays to shared config structs.
- Framebuffer-vs-logical: `gpu` context width/height are framebuffer pixels (Retina 2x); `Window::window_width/height` are logical. M2b does not render, so `display_tex` stays logical-sized with an `// M3: framebuffer-vs-logical decision` marker.
- Reference docs (read-only inputs, committed in-repo): `docs/superpowers/research/m2/m2a-carryovers.md` (mandatory requirements this plan implements), `docs/superpowers/research/m2/wgsl-drafts/translation-notes.md` (per-line rationale for the shader ports), `docs/superpowers/research/m2/graphics-api-inventory.md`.
- End every commit message with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

## File Structure

- `cpplib/graphics.h` / `cpplib/graphics.cpp` — Task 1 amendments (group-0 hardening, `unset_structured_buffer`, 3D clear assert)
- `cpplib/gpu/gpu_context.cpp` — Task 2 (log via `logging`, `[[noreturn]]` fatal)
- `cpplib/logging.h` — Task 2 (`[[noreturn]]`)
- `cpplib/ui_stub.cpp` — Task 2 (new; no-op `ui::` until M4)
- `shaders/cs_agents_propagate.wgsl`, `shaders/cs_field_decay.wgsl` — Task 3 (from drafts), `shaders/cs_density_histo.wgsl` — Task 3 (new port)
- `shaders/tests/probe3d.wgsl` — Task 4 (voxel probe/reduction kernel)
- `tests/shader_compile_tests.cpp` — Task 3; `tests/sim_kernel_tests.cpp` — Task 4
- `main.cpp` — Task 5 (the port; replaces `main_m1.cpp` as the target's source)
- `tests/gen_test_dataset.cpp` — Task 6 (synthetic dataset generator)
- `CMakeLists.txt` — Tasks 2, 3, 4, 5, 6

---

### Task 1: Graphics-layer hardening (M2a carryovers)

**Files:**
- Modify: `cpplib/graphics.h` (ComputeShader struct, one new function decl)
- Modify: `cpplib/graphics.cpp` (get_compute_shader_from_code, run_compute, clear_texture(3D), unset_structured_buffer)
- Test: `tests/graphics_tests.cpp` (extend), `tests/file_system_tests.cpp` (extend)

**Interfaces:**
- Consumes: M2a graphics layer at `dafa63f`.
- Produces: `ComputeShader.uses_group0 : bool`; `void unset_structured_buffer(uint32_t slot)`; hardened `run_compute` that binds group 0 iff the shader declares it and fatals (named) when a group-0 shader is dispatched with no uniform bound. Task 5's seven dispatch sites rely on these exact semantics.

The M2a final review confirmed (empirically) that `run_compute` binds group 0 from shadow-state presence, not the shader's layout — stale state silently invalidates dispatches. The fix: record at shader-creation time whether the WGSL declares a group-0 binding, and drive group-0 binding from that.

- [ ] **Step 1: Amend graphics.h**

In `struct ComputeShader` add one field after `valid`:

```cpp
    bool uses_group0;      // WGSL source declares @group(0) — drives uniform binding in run_compute
```

In the compute section of the namespace, next to `unset_texture_compute`, add:

```cpp
void unset_structured_buffer(uint32_t slot);
```

No other header changes.

- [ ] **Step 2: Implement in graphics.cpp**

In `get_compute_shader_from_code`, after the source is available and before returning, set:

```cpp
    // Comment-safe enough for our controlled sources: every sim shader that
    // uses a uniform declares it as literally "@group(0)".
    cs.uses_group0 = (strstr(code, "@group(0)") != NULL);
```

(`code` is the WGSL source parameter; `<cstring>` is already included.)

In `run_compute`, replace the group-0 binding condition. Current shape:

```cpp
    if (g_uniform_buffer) { /* build+set bind group 0 */ }
```

New shape:

```cpp
    if (g_compute_shader->uses_group0) {
        if (!g_uniform_buffer) {
            fatal("run_compute: shader declares @group(0) uniform but no constant buffer is bound (set_constant_buffer slot 0)");
        }
        /* build+set bind group 0 — existing code unchanged */
    }
    // Shader without @group(0): never bind group 0, even if shadow state holds a uniform.
```

(Match the file's existing fatal-error helper — the same one the limit gates use.)

Implement the alias next to the other unset functions:

```cpp
void unset_structured_buffer(uint32_t slot)
{
    assert(slot < MAX_SLOTS);
    g_compute_slots[slot] = {};
}
```

In `clear_texture(Texture3D *texture, float value)` add at the top (mirroring the 2D variants):

```cpp
    assert(texture->format == Format::R32_FLOAT);
```

- [ ] **Step 3: Extend graphics_tests.cpp**

Add Test 6 exercising both new semantics with the EXISTING test shaders (`test_write_ids.wgsl` declares `@group(0)`; `test_tex3d_roundtrip.wgsl` does not — verify this by reading both files; if the roundtrip shader has no uniform it is the no-group0 case):

```cpp
    // Test 6: group-0 binding driven by shader declaration, not shadow state.
    {
        // (a) Uniform still bound in shadow state from an earlier test is
        //     harmless for a shader that declares no @group(0).
        graphics::set_constant_buffer(&cfg_buffer, 0);      // deliberately stale
        graphics::set_compute_shader(&tex_shader);          // test_tex3d_roundtrip: no @group(0)
        assert(!tex_shader.uses_group0);
        graphics::set_texture_compute(&tex3d, 0);
        graphics::run_compute(1, 1, 1);                     // must NOT Dawn-error (M2a needed a manual reset here)
        graphics::unset_texture_compute(0);

        // (b) uses_group0 detection on a uniform-declaring shader.
        assert(ids_shader.uses_group0);

        // (c) unset_structured_buffer clears a slot (rebind + dispatch still valid).
        graphics::set_compute_shader(&ids_shader);
        graphics::set_constant_buffer(&cfg_buffer, 0);
        graphics::set_structured_buffer(&out_buffer, 2);
        graphics::unset_structured_buffer(2);
        graphics::set_structured_buffer(&out_buffer, 2);
        graphics::run_compute(1, 1, 1);
        graphics::unset_structured_buffer(2);
    }
    printf("Test 6 (group0 hardening + unset_structured_buffer) passed\n");
```

Adapt variable names to the file's existing ones (the shaders/buffers already exist in the test; reuse them — do not create new pipelines needlessly). Remove the now-unneeded workaround comment/reset (`set_constant_buffer` with an empty `ConstantBuffer{}`) that Test 2 used, since (a) proves it obsolete — but keep Test 2's assertions unchanged.

- [ ] **Step 4: Extend file_system_tests.cpp**

In the existing round-trip test, after size assertions add the NUL-contract assert (the M2a carryover):

```cpp
    assert(((char *)f.data)[f.size] == 0);  // extra NUL byte: shader sources are consumed as C strings
```

- [ ] **Step 5: Build + run all tests**

Run: `nice -n 19 cmake --build build -j 8 && ctest --test-dir build`
Expected: all suites pass (cpplib_tests, file_system_tests, graphics_tests).

- [ ] **Step 6: Commit**

```bash
git add cpplib/graphics.h cpplib/graphics.cpp tests/graphics_tests.cpp tests/file_system_tests.cpp
git commit -m "graphics: derive group-0 binding from shader, add unset_structured_buffer + guards"
```

---

### Task 2: Enable cpplib modules, route Dawn errors through logging, ui stub

**Files:**
- Modify: `CMakeLists.txt` (uncomment memory/random/logging; add ui_stub.cpp)
- Modify: `cpplib/logging.h` (`[[noreturn]]`), `cpplib/gpu/gpu_context.cpp` (fprintf → logging)
- Create: `cpplib/ui_stub.cpp`

**Interfaces:**
- Consumes: `cpplib/logging.{h,cpp}` (existing, currently uncompiled), `cpplib/ui.h` (existing, unchanged).
- Produces: `memory::alloc_heap`, `random::uniform`, `logging::print_error` available to main.cpp; a link-complete no-op `ui::` surface. Task 5 links against all of these.

- [ ] **Step 1: CMake — enable modules**

In the source list, uncomment the three `# enabled in M2` lines so they read:

```cmake
  cpplib/memory.cpp
  cpplib/random.cpp
  cpplib/logging.cpp
```

and add `cpplib/ui_stub.cpp` beside them. (These files have no Win32 dependencies — verified by grep; they compile as-is.) Build to confirm before proceeding: `cmake -B build && nice -n 19 cmake --build build -j 8`. Fix any trivial POSIX issues that surface (report them; expected: none).

- [ ] **Step 2: logging.h `[[noreturn]]`**

Read `cpplib/logging.{h,cpp}` first. Add `[[noreturn]]` to the fatal-path function's declaration (and definition if the attribute placement requires it). If logging.h has no fatal function (only print/error), instead annotate the `fatal` helper where it lives (grep for its definition — M2a placed one in gpu_context.cpp/graphics.cpp) and skip logging.h. Whichever function actually terminates gets the attribute.

- [ ] **Step 3: gpu_context.cpp error routing**

Replace the raw `fprintf(stderr, ...)` calls in the Dawn error/device-lost callbacks and the limit logging with the corresponding `logging::` functions (read logging.h for the exact names — it is a thin printf-style wrapper). Keep message text identical. The fatal gates keep aborting.

- [ ] **Step 4: ui_stub.cpp**

Create `cpplib/ui_stub.cpp` — a complete no-op implementation of `cpplib/ui.h` (real ImGui arrives in M4):

```cpp
// M4: replace with the Dear ImGui implementation (see docs/superpowers/research/m2/imgui-integration.md).
#include "ui.h"

namespace ui {

static bool g_input_responsive = false;

void init(float, float) {}
void draw_text(const char *, Font *, float, float, Vector4, Vector2) {}
void draw_text(const char *, Font *, Vector2, Vector4, Vector2) {}
void draw_text(const char *, Vector2, Vector4, Vector2) {}
void draw_rect(float, float, float, float, Vector4) {}
void draw_rect(Vector2, float, float, Vector4) {}
Panel start_panel(char *name, Vector2 pos, float width) { Panel p = {}; p.name = name; p.pos = pos; p.width = width; return p; }
Panel start_panel(char *name, float x, float y, float width) { return start_panel(name, Vector2(x, y), width); }
void end_panel(Panel *) {}
Vector4 get_panel_rect(Panel *) { return Vector4(0, 0, 0, 0); }
void end() {}
bool add_toggle(Panel *, char *, bool *) { return false; }
bool add_slider(Panel *, char *, float *, float, float) { return false; }
void release() {}
void set_input_responsive(bool is_responsive) { g_input_responsive = is_responsive; }
bool is_input_responsive() { return g_input_responsive; }
bool is_registering_input() { return false; }
float get_screen_width() { return 0.0f; }
Font *get_font() { return NULL; }

}
```

(Default-argument values live in the header only; definitions must not repeat them. `ui.cpp` and `font.cpp` stay uncompiled — do not delete them; M4 revisits.)

- [ ] **Step 5: Build + tests**

Run: `nice -n 19 cmake --build build -j 8 && ctest --test-dir build`
Expected: clean build, all tests pass, `./build/polyphorm` (still main_m1) runs and the granted-limits lines now come through `logging`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt cpplib/logging.h cpplib/gpu/gpu_context.cpp cpplib/ui_stub.cpp
git commit -m "cpplib: enable memory/random/logging, route Dawn errors via logging, add ui stub"
```

---

### Task 3: Sim shaders land in shaders/ + compile-validation test

**Files:**
- Create: `shaders/cs_agents_propagate.wgsl` (from `docs/superpowers/research/m2/wgsl-drafts/cs_agents_propagate.wgsl`)
- Create: `shaders/cs_field_decay.wgsl` (from `docs/superpowers/research/m2/wgsl-drafts/cs_field_decay.wgsl`)
- Create: `shaders/cs_density_histo.wgsl` (new port, full code below)
- Create: `tests/shader_compile_tests.cpp`
- Modify: `CMakeLists.txt` (new test target)

**Interfaces:**
- Consumes: `graphics::get_compute_shader_from_code` (error-scope validated, `valid=false` on Tint error), `file_system::read_file` (NUL-terminated), `gpu::init_device`.
- Produces: the three production WGSL files Task 5 loads at runtime, proven to compile on the pinned Dawn. Bind layouts: propagate g1 = {0:deposit rw-tex, 1:trace rw-tex, 2..7:particle f32 buffers}; decay g1 = {0:tex_in read, 1:tex_out write, 2:trace rw}; histo g1 = {0:trace rw-tex, 1:histogram atomic u32 buffer, 2..4:particles xyz, 5:weights, 6:halos_densities}. All three declare the full uniform at `@group(0) @binding(0)`.

- [ ] **Step 1: Copy the two drafts verbatim**

```bash
cp docs/superpowers/research/m2/wgsl-drafts/cs_agents_propagate.wgsl shaders/cs_agents_propagate.wgsl
cp docs/superpowers/research/m2/wgsl-drafts/cs_field_decay.wgsl shaders/cs_field_decay.wgsl
```

Do NOT edit them beyond what Step 4's compile loop forces. The drafts encode 17 quirk toggles and an exact RNG draw order; every deviation must be justified against `translation-notes.md` and documented in your report.

- [ ] **Step 2: Write cs_density_histo.wgsl**

Port of `shaders/cs_density_histo.hlsl` (98 lines; read it side-by-side). Create `shaders/cs_density_histo.wgsl`:

```wgsl
// Port of cs_density_histo.hlsl — bins the trace density at data-point
// positions into a log histogram. Bug-for-bug: RNG constants and the
// seed-guard typo (m_w tested where m_z is meant) are preserved verbatim.
//
// Bind contract (see graphics:: convention):
//   @group(0) @binding(0)  StatisticsConfig uniform (32 B)
//   @group(1) @binding(0)  trace texture   (r32float, read_write — matches RWTexture3D)
//   @group(1) @binding(1)  histogram       (atomic<u32> storage)
//   @group(1) @binding(2..4) particles x/y/z (f32 storage)
//   @group(1) @binding(5)  particles_weights
//   @group(1) @binding(6)  halos_densities

override WG_X: u32 = 10u;
override WG_Y: u32 = 10u;
override WG_Z: u32 = 10u;

struct StatisticsConfig {
    n_data_points: i32,
    n_histo_bins: i32,
    histogram_base: f32,
    sample_randomly: i32,
    world_width: f32,
    world_height: f32,
    world_depth: f32,
    filler3: i32,
};

@group(0) @binding(0) var<uniform> cfg: StatisticsConfig;

@group(1) @binding(0) var tex_density: texture_storage_3d<r32float, read_write>;
@group(1) @binding(1) var<storage, read_write> histogram: array<atomic<u32>>;
@group(1) @binding(2) var<storage, read_write> particles_x: array<f32>;
@group(1) @binding(3) var<storage, read_write> particles_y: array<f32>;
@group(1) @binding(4) var<storage, read_write> particles_z: array<f32>;
@group(1) @binding(5) var<storage, read_write> particles_weights: array<f32>;
@group(1) @binding(6) var<storage, read_write> halos_densities: array<f32>;

const BAD_W: u32 = 0x464fffffu;
const BAD_Z: u32 = 0x9068ffffu;

struct RNG { m_w: u32, m_z: u32, };

fn rng_set_seed(rng: ptr<function, RNG>, seed1: u32, seed2: u32) {
    (*rng).m_w = seed1;
    (*rng).m_z = seed2;
    // QUIRK(rng_seed_guard_typo): second guard tests m_w where m_z is meant,
    // exactly as the HLSL does. A zero m_z is never repaired.
    if ((*rng).m_w == 0u || (*rng).m_w == BAD_W) { (*rng).m_w = (*rng).m_w + 1u; }
    if ((*rng).m_w == 0u || (*rng).m_z == BAD_Z) { (*rng).m_z = (*rng).m_z + 1u; }
}

fn rng_random_uint(rng: ptr<function, RNG>) -> u32 {
    (*rng).m_z = 36969u * ((*rng).m_z & 65535u) + ((*rng).m_z >> 16u);
    (*rng).m_w = 18000u * ((*rng).m_w & 65535u) + ((*rng).m_w >> 16u);
    return ((*rng).m_z << 16u) + (*rng).m_w;
}

fn rng_random_float(rng: ptr<function, RNG>) -> f32 {
    return f32(rng_random_uint(rng)) / f32(0xFFFFFFFFu);
}

fn wang_hash(seed_in: u32) -> u32 {
    var seed = (seed_in ^ 61u) ^ (seed_in >> 16u);
    seed = seed * 9u;
    seed = seed ^ (seed >> 4u);
    seed = seed * 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

@compute @workgroup_size(WG_X, WG_Y, WG_Z)
fn main(@builtin(local_invocation_index) thread_index: u32,
        @builtin(workgroup_id) group_id: vec3<u32>,
        @builtin(num_workgroups) num_groups: vec3<u32>) {
    // Generalised flat index; reduces to thread_index + 1000*(gx + gy*10 + gz*100)
    // for the production dispatch (10,10,gz) — same bijection as the HLSL.
    let group_idx = group_id.x + group_id.y * num_groups.x + group_id.z * num_groups.x * num_groups.y;
    let idx = thread_index + (WG_X * WG_Y * WG_Z) * group_idx;

    if (idx >= u32(cfg.n_data_points)) { // Only consider halo/galaxy locations
        return;
    }

    var x = particles_x[idx];
    var y = particles_y[idx];
    var z = particles_z[idx];

    if (cfg.sample_randomly != 0) {
        var rng: RNG;
        rng_set_seed(&rng, wang_hash(73u * idx), wang_hash(u32(x * y * z)));
        x = rng_random_float(&rng) * cfg.world_width;
        y = rng_random_float(&rng) * cfg.world_height;
        z = rng_random_float(&rng) * cfg.world_depth;
    }

    let density = textureLoad(tex_density, vec3<i32>(vec3<u32>(vec3<f32>(x, y, z)))).x;
    halos_densities[idx] = density;

    var histo_index = 0u;
    if (density > 1.0e-5) {
        let log_density = log(density) / log(cfg.histogram_base);
        // f32->u32 saturates in WGSL; D3D GPU hardware saturates too — matching.
        histo_index = 1u + min(u32(log_density + 5.0), u32(cfg.n_histo_bins - 3));
    }

    atomicAdd(&histogram[histo_index], 1u);
    atomicMax(&histogram[u32(cfg.n_histo_bins - 1)], u32(1.0e5 * density));
}
```

Note the deliberate mirror of the HLSL: `mass` is read in the HLSL but unused (its normalization line is commented out) — the WGSL binds `particles_weights` at slot 5 to keep the bind contract identical even though the value is unused; read it into `_ = particles_weights[idx];` if Tint warns about an unused binding, otherwise the declaration alone is fine (Dawn requires bound slots to match declarations, and main.cpp binds slot 5 — the declaration must stay).

- [ ] **Step 3: Write the compile-validation test**

`tests/shader_compile_tests.cpp`:

```cpp
// Headless: every production WGSL shader must compile on the pinned Dawn.
// Catches the "uncertain lines" from translation-notes.md §7 at ctest time
// instead of at app startup.
#include "../cpplib/gpu/gpu_context.h"
#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
#include <cstdio>

static void check_compiles(const char *path)
{
    File f = file_system::read_file(path);
    assert(f.data != NULL);
    ComputeShader cs = graphics::get_compute_shader_from_code((char *)f.data, f.size);
    if (!graphics::is_ready(&cs)) {
        printf("FAILED to compile %s (see Dawn error above)\n", path);
        assert(false);
    }
    printf("compiled OK: %s (uses_group0=%d)\n", path, (int)cs.uses_group0);
    graphics::release(&cs);
    file_system::release_file(f);
}

int main()
{
    GpuContext ctx;
    gpu::init_device(&ctx);
    graphics::init_context(&ctx);

    check_compiles(SHADER_DIR "/cs_agents_propagate.wgsl");
    check_compiles(SHADER_DIR "/cs_field_decay.wgsl");
    check_compiles(SHADER_DIR "/cs_density_histo.wgsl");

    printf("All shader compile tests passed\n");
    return 0;
}
```

(`graphics::init_context` is whatever M2a's graphics_tests.cpp calls to attach the GpuContext — read that file and use the same init sequence verbatim. `SHADER_DIR` comes from CMake below.)

- [ ] **Step 4: CMake target + iterate until green**

Add to CMakeLists.txt, modeled exactly on the existing `graphics_tests` target (same sources list pattern, `-UNDEBUG`, `cxx_std_17`):

```cmake
add_executable(shader_compile_tests tests/shader_compile_tests.cpp
  cpplib/graphics.cpp cpplib/gpu/gpu_context.cpp cpplib/file_system.cpp cpplib/platform.cpp cpplib/logging.cpp)
target_compile_definitions(shader_compile_tests PRIVATE SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")
```

(Adjust the source list to match what graphics_tests actually links — copy its list.) Then build and run. If a draft fails to compile, apply ONLY the pre-authorized escape hatches from translation-notes §7, in this order of preference, and document each in your report:
1. `f32(0xFFFFFFFFu)` → literal `4294967296.0` (identical value)
2. function-scope `const` → `let`
3. `texture_storage_3d<r32float, read>` → `read_write` (wider access, same behavior)
4. `_ = textureLoad(...)` phony assignment → `let _unused = textureLoad(...)` or a plain discarded call, whichever Tint accepts
5. reserved-word rename (keep a `// was:` comment)

Any OTHER compile error means the draft has a real bug: fix minimally, cite the HLSL line it mirrors, and flag it prominently in your report for review.

Run: `ctest --test-dir build -R shader_compile_tests`
Expected: PASS with three "compiled OK" lines, `uses_group0=1` for all three.

- [ ] **Step 5: Commit**

```bash
git add shaders/cs_agents_propagate.wgsl shaders/cs_field_decay.wgsl shaders/cs_density_histo.wgsl tests/shader_compile_tests.cpp CMakeLists.txt
git commit -m "shaders: land WGSL sim kernels (propagate, decay, histo) + compile validation test"
```

---

### Task 4: Decay-boundary and OOB-store micro-tests

**Files:**
- Create: `shaders/tests/probe3d.wgsl`
- Create: `tests/sim_kernel_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3's `shaders/cs_field_decay.wgsl` and `cs_agents_propagate.wgsl`; the graphics compute/readback machinery.
- Produces: regression pins for QUIRK(decay_weight_all_int3), QUIRK(nonperiodic_low_boundary), the uncompensated-denominator darkening, OOB-load-returns-0, and OOB-store-is-discarded. These four quirks are invisible in a rendered image (translation-notes §7.9) — this test is the only guard the port has.

- [ ] **Step 1: Probe kernel**

`shaders/tests/probe3d.wgsl` — reads N voxels (coords in a storage buffer) and sums a z-slab, writing results to an output buffer:

```wgsl
// Test-only: probe voxels of a 3D r32float texture and sum one x-slab.
struct ProbeCfg {
    n_probes: i32,
    slab_x: i32,      // x index of the slab to sum; -1 = skip
    dim_y: i32,
    dim_z: i32,
};
@group(0) @binding(0) var<uniform> cfg: ProbeCfg;
@group(1) @binding(0) var tex: texture_storage_3d<r32float, read_write>;
@group(1) @binding(1) var<storage, read_write> coords: array<i32>;   // xyz triples
@group(1) @binding(2) var<storage, read_write> results: array<f32>;  // n_probes values + [n_probes] = slab sum

@compute @workgroup_size(1, 1, 1)
fn main() {
    for (var i = 0; i < cfg.n_probes; i = i + 1) {
        let c = vec3<i32>(coords[3*i], coords[3*i+1], coords[3*i+2]);
        results[i] = textureLoad(tex, c).x;
    }
    if (cfg.slab_x >= 0) {
        var sum = 0.0;
        for (var y = 0; y < cfg.dim_y; y = y + 1) {
            for (var z = 0; z < cfg.dim_z; z = z + 1) {
                sum = sum + textureLoad(tex, vec3<i32>(cfg.slab_x, y, z)).x;
            }
        }
        results[cfg.n_probes] = sum;
    }
}
```

- [ ] **Step 2: The test driver**

`tests/sim_kernel_tests.cpp`. Same headless init as Task 3's test. Grid 16×16×16.

Test A — decay boundary (pins D2+D4+D5+A4 simultaneously, values derived in translation-notes §7.9):

```cpp
    // SimulationConfig must byte-match main.cpp's (16 scalars, 64 B).
    struct SimulationConfig {
        float sense_spread, sense_distance, turn_angle, move_distance;
        float deposit_value, decay_factor, center_attraction; int world_width;
        int world_height, world_depth; float move_sense_coef, normalization_factor;
        int n_data_points, n_agents, n_iteration, filler3;
    };
    static_assert(sizeof(SimulationConfig) == 64, "config layout");

    const int W = 16;
    const float DECAY = 0.9f;

    Texture3D tex_in  = graphics::get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
    Texture3D tex_out = graphics::get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
    Texture3D trace   = graphics::get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
    graphics::clear_texture(&tex_in, 1.0f);
    graphics::clear_texture(&tex_out, 0.0f);
    graphics::clear_texture(&trace, 0.0f);

    SimulationConfig cfg = {};
    cfg.decay_factor = DECAY;
    cfg.world_width = W; cfg.world_height = W; cfg.world_depth = W;
    ConstantBuffer cfg_buf = graphics::get_constant_buffer(sizeof(SimulationConfig));
    graphics::update_constant_buffer(&cfg_buf, &cfg);

    File f = file_system::read_file(SHADER_DIR "/cs_field_decay.wgsl");
    ComputeShader decay = graphics::get_compute_shader_from_code((char *)f.data, f.size);
    file_system::release_file(f);
    assert(graphics::is_ready(&decay));

    graphics::set_compute_shader(&decay);
    graphics::set_constant_buffer(&cfg_buf, 0);
    graphics::set_texture_compute(&tex_in, 0);
    graphics::set_texture_compute(&tex_out, 1);
    graphics::set_texture_compute(&trace, 2);
    graphics::run_compute(W / 8, W / 8, W / 8);
    graphics::unset_texture_compute(0);
    graphics::unset_texture_compute(1);
    graphics::unset_texture_compute(2);

    // Probe tex_out: interior (8,8,8), low face (0,8,8), high face (15,8,8).
    // Expected (translation-notes §7.9):
    //   interior: 1.0 * DECAY                      (all 27 taps present)
    //   x=0 face: (23.6188021-7.3094011)/23.6188021 * DECAY = 0.6905262 * DECAY
    //   x=W-1:    1.0 * DECAY                      (high side wraps)
    float vals[4] = {0, 0, 0, 0};
    probe(&tex_out, /*coords*/ {8,8,8, 0,8,8, 15,8,8}, /*slab_x*/ -1, vals);
    assert(approx(vals[0], 1.0f * DECAY, 1e-5f));
    assert(approx(vals[1], 0.6905262f * DECAY, 1e-5f));
    assert(approx(vals[2], 1.0f * DECAY, 1e-5f));
```

Write the small `probe(...)` helper in the test file: it loads `shaders/tests/probe3d.wgsl` once, uploads coords via a StructuredBuffer, binds the target texture at slot 0 / coords at 1 / results at 2 with a ProbeCfg uniform, dispatches (1,1,1), and reads results back via `capture_structured_buffer`. `approx(a, b, eps)` is `fabsf(a-b) <= eps`. Follow the full bind discipline (set cfg at 0, unset all group-1 slots after).

Test B — OOB store is discarded, not clamped (translation-notes §7.8): run one propagate step with a single "agent" whose deposit write coordinate lands exactly at `x == W` (one past the end) and assert the far slab received nothing.

```cpp
    // One particle, positioned so mod_floor(x - move, W) == W exactly:
    // x = -1e-7 wraps to W under floor-mod at f32 precision.
    // With move_distance=0 and turn angles 0, the deposit write coord is
    // uint3(x_wrapped, y, z) -> (W, 8, 8) -> OOB store, discarded per WGSL.
    // If Metal clamped instead, slab x=W-1 would gain deposit_value * weight.
```

Setup: particle buffers of length 1. Use the DATA-POINT path (simplest deterministic write): `theta = -5.0f` is the data-point marker (see main.cpp's update_particles and the draft's data-point branch — read the draft to confirm the branch condition and its deposit-write line before coding). Fields: `x = -1e-7f`, `y = 8.0f`, `z = 8.0f`, `phi = 0`, `theta = -5.0f`, `weight = 1.0f`. Cfg: `n_data_points = 1`, `n_agents = 0`, `deposit_value = 7.0f`, `move_distance = 0`, `sense_distance = 0`, `sense_spread = 0`, `turn_angle = 0`, `move_sense_coef = 1.0f`, `normalization_factor = 1.0f`, world dims W. Clear deposit+trace to 0. Data points deposit at their own position — which is the crafted near-zero-negative x that floor-mod may wrap to exactly `W` (the OOB edge). Dispatch `run_compute(1, 1, 1)`; the shader's index guard makes the other 999 invocations return. Then probe the deposit texture: assert slab `x = W-1` sums to `0.0f`. If `7.0` appears at x=W-1, Metal CLAMPED the OOB store — a real quirk divergence vs D3D11's discard: do NOT adjust the test to pass; report BLOCKED with the evidence (this changes M5's validation calculus and the human decides).

If crafting the exact `mod_floor == W` edge proves impossible at f32 precision on this machine (the rounding is hardware-dependent), fall back to asserting the well-defined case: particle at `x = -1e-7f` must deposit at EITHER x==0 (mod gave 0) or nowhere (mod gave W, discarded) but NEVER at x==W-1 — three-way probe, assert `slab[W-1] == 0`. Document which case the hardware produced.

- [ ] **Step 3: CMake + run**

Add `sim_kernel_tests` target (copy the shader_compile_tests pattern, same SHADER_DIR define). Run `ctest --test-dir build -R sim_kernel_tests`. Iterate; the expected values are exact math, tolerance 1e-5.

- [ ] **Step 4: Commit**

```bash
git add shaders/tests/probe3d.wgsl tests/sim_kernel_tests.cpp CMakeLists.txt
git commit -m "tests: pin decay boundary quirks (D2/D4/D5/A4) and OOB-store discard on GPU"
```

---

### Task 5: main.cpp port

**Files:**
- Modify: `main.cpp` (the port — region-by-region edits below)
- Modify: `CMakeLists.txt` (target sources: `main_m1.cpp` → `main.cpp`)
- Delete: `main_m1.cpp`

**Interfaces:**
- Consumes: everything above. `gpu::init(Window*)` for windowed init; Format enums from graphics.h; the three WGSL shaders; ui stub.
- Produces: a `polyphorm` binary that opens the M1 window and runs propagate → decay → histogram every frame with correct energy statistics. Rendering is intentionally absent (M3/M4): the window shows the clear color; the sim runs underneath. Task 6 adds headless mode on top of THIS structure — keep the frame loop shape intact.

Work through the regions in order. Everything not mentioned stays byte-identical — this is a port, not a rewrite. Build frequently.

- [ ] **Step 1: Includes and Windows-isms (lines 1-18)**

Remove `#include <mmsystem.h>`, both `#pragma warning` lines. Keep `font.h` include but remove the `font::init();` call later (line 441) — the stub ui doesn't need fonts (`// M4: font::init returns with ImGui`). Add `#include "gpu/gpu_context.h"` if graphics.h doesn't already pull it in (check).

- [ ] **Step 2: Config structs — layout guards (after line 254 / 305 / 317)**

Immediately after `struct SimulationConfig { ... };` add:

```cpp
static_assert(sizeof(SimulationConfig) == 64, "SimulationConfig must stay 16 4-byte scalars (WGSL uniform layout match)");
static_assert(offsetof(SimulationConfig, deposit_value) == 16, "layout drift");
static_assert(offsetof(SimulationConfig, world_height) == 32, "layout drift");
static_assert(offsetof(SimulationConfig, n_data_points) == 48, "layout drift");
static_assert(offsetof(SimulationConfig, filler3) == 60, "layout drift");
```

After `RenderingConfig`: `static_assert(sizeof(RenderingConfig) == 3 * 64 + 36 * 4, "RenderingConfig layout");`
After `StatisticsConfig`: `static_assert(sizeof(StatisticsConfig) == 32, "StatisticsConfig layout");`
Add `#include <cstddef>` for offsetof.

- [ ] **Step 3: Graphics init (lines 436-450)**

Replace `graphics::init(); graphics::init_swap_chain(&window);` with the M2a lifecycle (read graphics.h for the exact init names — they are the ones main_m1.cpp / graphics_tests use):

```cpp
    GpuContext gpu_ctx = gpu::init(&window);
    graphics::init_context(&gpu_ctx);   // exact name per graphics.h
```

Drop `font::init();`. Keep `ui::init(...)` (stub). Keep the render-target/depth-buffer block as is (all implemented or stubbed in M2a).

- [ ] **Step 4: Shader loading (lines 452-561) — full replacement**

Replace the entire shader-loading section with:

```cpp
    // M2b: only the simulation kernels are ported to WGSL. Render-path
    // shaders return in M3 (particles) and M4 (volume/PT).
    auto load_compute = [](const char *path) {
        File f = file_system::read_file(path);
        if (!f.data) { logging::print_error("shader file missing: %s", path); assert(false); }
        ComputeShader cs = graphics::get_compute_shader_from_code((char *)f.data, f.size);
        file_system::release_file(f);
        assert(graphics::is_ready(&cs));
        printf("%s compiled...\n", path);
        return cs;
    };
    ComputeShader compute_shader = load_compute("shaders/cs_agents_propagate.wgsl");
    ComputeShader decay_compute_shader = load_compute("shaders/cs_field_decay.wgsl");
    ComputeShader cs_density_histo = load_compute("shaders/cs_density_histo.wgsl");

    // M3: cs_particles_transform, cs_particles_blit, cs_agents_sort (upstream default-off)
    // M4: cs_volpath + all vs_/ps_ shaders
    ComputeShader draw_compute_shader_particle = {};
    ComputeShader blit_compute_shader = {};
    ComputeShader sort_shader = {};
    ComputeShader cs_volpath = {};
    VertexShader vertex_shader = {}, vertex_shader_2d = {};
    PixelShader pixel_shader = {}, pixel_shader_2d = {}, ps_volume_highlight = {},
                ps_volume_halocolor = {}, ps_volume_overdensity = {}, ps_volume_velocity = {}, ps_volpath = {};
```

(Adjust `logging::print_error` to logging.h's real function name. If graphics.h's `get_compute_shader_from_code` signature takes `(char*, uint32_t)` keep the call as upstream had it.)

- [ ] **Step 5: Textures and samplers (lines 563-586)**

Keep the `#ifdef HALO_COLOR_ANALYSIS` / `VELOCITY_ANALYSIS` structure (both are off) but replace the live lines:

```cpp
    Texture3D trail_tex_A = graphics::get_texture3D(NULL, GRID_RESOLUTION_X, GRID_RESOLUTION_Y, GRID_RESOLUTION_Z, Format::R32_FLOAT, 4);
    Texture3D trail_tex_B = graphics::get_texture3D(NULL, GRID_RESOLUTION_X, GRID_RESOLUTION_Y, GRID_RESOLUTION_Z, Format::R32_FLOAT, 4);
    Texture3D trace_tex  = graphics::get_texture3D(NULL, GRID_RESOLUTION_X, GRID_RESOLUTION_Y, GRID_RESOLUTION_Z, Format::R32_FLOAT, 4);
```

with a comment: `// QUIRK(r16f_channel_truncation): upstream R16_FLOAT single-channel; r32float is the exact-match WebGPU storage format (translation-notes §0). VAC scale: ~2.5 GB each.` In the dead `#ifdef` arms, keep upstream's lines but behind the ifdef they never compile — replace their DXGI formats with `Format::R32_FLOAT` anyway so the file has no DXGI tokens left, and add `// M-later: widen format for this analysis mode`.

Display textures (logical-sized; M3 revisits Retina):

```cpp
    Texture2D display_tex = graphics::get_texture2D(NULL, window_width, window_height, Format::R32G32B32A32_FLOAT, 16);      // M3: framebuffer-vs-logical decision
    Texture2D display_tex_uint = graphics::get_texture2D(NULL, window_width, window_height, Format::R32_UINT, 4);
```

Samplers: `graphics::get_texture_sampler(SampleMode::CLAMP, Filter::ANISOTROPIC)` — use the fork enums from graphics.h (read it; the parameterless overload stays).

- [ ] **Step 6: F2/F8 clear sites (lines 923-926, 935-938)**

```cpp
                graphics::clear_texture(&trail_tex_A, 0.0f);
                graphics::clear_texture(&trail_tex_B, 0.0f);
                graphics::clear_texture(&trace_tex, 0.0f);
```

and for F8 just the trace line. Delete the `float clear_tex[4]` locals.

- [ ] **Step 7: The seven dispatch sites — full bind discipline**

This is the load-bearing step (carryovers I3/I4). Only three sites are live in M2b; the other four are guarded off with the shaders they need.

Site 1 — propagate (lines 986-1006). The uniform set at line 982-983 stays (it IS this dispatch's group-0). Add the missing buffer unsets:

```cpp
            graphics::run_compute(10, 10, grid_z);
            graphics::unset_texture_compute(0);
            graphics::unset_texture_compute(1);
            graphics::unset_structured_buffer(2);
            graphics::unset_structured_buffer(3);
            graphics::unset_structured_buffer(4);
            graphics::unset_structured_buffer(5);
            graphics::unset_structured_buffer(6);
            graphics::unset_structured_buffer(7);
```

Site 2 — sort (lines 1009-1025): wrap the whole block's condition as `if (run_mold && sort_agents && graphics::is_ready(&sort_shader))` and add `// M3+: cs_agents_sort not yet ported (upstream toggle is commented out; default off)`. Body unchanged otherwise.

Site 3 — decay (lines 1028-1043): before `run_compute`, re-assert the sim uniform (the sort loop may have updated it; decay's shader declares the same group-0):

```cpp
            graphics::set_constant_buffer(&config_buffer, 0);
```

after the existing `set_texture_compute` calls. Unsets already present and sufficient (0,1,2; no buffers bound).

Site 4 — histogram (lines 1046-1068): it already sets `statistics_config_buffer` at slot 0 (line 1055) — keep, and complete the unsets:

```cpp
            graphics::unset_texture_compute(0);
            graphics::unset_structured_buffer(1);
            graphics::unset_structured_buffer(2);
            graphics::unset_structured_buffer(3);
            graphics::unset_structured_buffer(4);
            graphics::unset_structured_buffer(5);
            graphics::unset_structured_buffer(6);
```

IMPORTANT after this block: the NEXT frame's propagate relies on `set_constant_buffer(&config_buffer, 0)` at line 983 running every frame — verify it does (it is unconditional). Good.

Site 5 — particle draw + Site 6 — blit (lines 1165-1193): guard the compute portion:

```cpp
            if(vis_mode == VisualizationMode::VM_PARTICLES) {
                // M3: particle draw path (cs_particles_transform + cs_particles_blit not yet ported).
                // The quad draw below is a draw_mesh stub no-op until then.
```

Delete the `ClearUnorderedAccessViewUint` line and both dispatch groups (set/run/unset for `draw_compute_shader_particle` and `blit_compute_shader`); KEEP the `update_constant_buffer(&rendering_settings_buffer, ...)` line and the vs/ps/quad_mesh calls (they hit M2a stubs and warn once). `display_tex_uint` stays allocated.

Site 7 — volpath (lines 1255-1293): guard the compute portion with `if (run_pt && rendering_config.pt_iteration < 1e5 && graphics::is_ready(&cs_volpath))` — everything else in the VM_PATH_TRACING block stays (stub draws).

- [ ] **Step 8: Rendering-adjacent leftovers**

- Line 1162-1163 (`set_render_targets_viewport` + `clear_render_target`): keep — implemented in M2a; this gives the M1-style cleared window.
- Histogram DRAWING block (from `graphics::set_render_targets_viewport` at line 1317 through the `ui::end()` at line 1440): keep as-is — `ui::` calls are stub no-ops. The STATISTICS portion above it (capture_structured_buffer, mean/energy computation, lines 1300-1315) is live and load-bearing for Task 6.
- Screenshot block (1444-1453): keep; stubs.
- `capture\\frame` (line 1447): change to `"capture/frame"`.
- Cleanup block (1629-1667): keep; add `graphics::release(&ps_volume_velocity); graphics::release(&ps_volpath); graphics::release(&cs_volpath);` only if upstream misses them (check; add missing releases for symmetry), and drop `graphics::release(&tex_sampler_display)` if it was never in the list (it is at line — verify; upstream forgets it: add it).

- [ ] **Step 9: CMake swap + build until clean**

In CMakeLists.txt replace `main_m1.cpp` with `main.cpp`; `git rm main_m1.cpp`. Build: `cmake -B build && nice -n 19 cmake --build build -j 8`. Iterate on compile errors — expected leftovers are D3D11 enum tokens (`CLAMP`, `D3D11_FILTER_ANISOTROPIC`) and any function-name drift vs graphics.h; resolve against graphics.h, never by editing graphics.h.

- [ ] **Step 10: Windowed smoke run (no dataset yet)**

`./build/polyphorm` from the repo root will exit early with "Config file missing" — that IS the correct behavior (no config.polyp in repo). Create a throwaway one to smoke-test interactively:

```bash
cd /tmp && mkdir -p polytest && cd polytest
printf 'NUM_AGENTS=100000\nGRID_RESOLUTION=64\nGRID_PADDING=0.2\nSCREEN_X=1280\nSCREEN_Y=720\nCAMERA_FOV=45\nHISTOGRAM_BASE=2.0\n' > config.polyp
```

It will then fail on the missing dataset — also correct. Full run happens in Task 6 with the generator. For THIS task the deliverable is: clean build, clean startup to the two config guards, all three prior test suites still green (`ctest --test-dir build`).

- [ ] **Step 11: Commit**

```bash
git add main.cpp CMakeLists.txt
git rm main_m1.cpp
git commit -m "port: main.cpp on graphics:: layer — sim dispatches live, render paths gated to M3/M4"
```

---

### Task 6: Headless mode + synthetic dataset + energy-rising smoke test

**Files:**
- Modify: `main.cpp` (headless flag)
- Create: `tests/gen_test_dataset.cpp`
- Modify: `CMakeLists.txt` (generator target + the smoke ctest)

**Interfaces:**
- Consumes: Task 5's main.cpp structure.
- Produces: `polyphorm --headless N` (exit 0 iff energy rose); `gen_test_dataset <dir>` writing `config.polyp`, `<name>_metadata.txt`, `<name>.bin`; ctest `energy_smoke`. This is M2b's acceptance test (spec's "energy rising" criterion).

- [ ] **Step 1: Headless flag in main.cpp**

At the top of `main`, parse argv:

```cpp
    int headless_frames = 0;   // 0 = windowed
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0 && i + 1 < argc) headless_frames = atoi(argv[i + 1]);
    }
    const bool headless = headless_frames > 0;
```

Then guard, in order (each is a small `if (!headless)` or conditional-init):
- Window creation + `is_window_valid` assert → headless: skip both; declare `Window window = {};`.
- gpu init: `GpuContext gpu_ctx; if (headless) { gpu::init_device(&gpu_ctx); } else { gpu_ctx = gpu::init(&window); } graphics::init_context(&gpu_ctx);`
- `ui::init/set_input_responsive` → skip when headless.
- Render target / depth buffer block → skip when headless (declare empty structs; every use of them below is inside `if (!headless)` guards added next).
- In the frame loop: the window-title block, event loop, input-reaction block → `if (!headless)`. Headless instead: `if (simulation_config.n_iteration >= headless_frames) is_running = false;`
- The entire Rendering block (line 1160's brace) and the histogram DRAWING sub-block (from `set_render_targets_viewport` at 1317 to `ui::end()`) and the UI panel block and screenshot blocks → `if (!headless)`. The histogram STATISTICS portion (capture + mean/energy math) runs in BOTH modes — it is the metric.
- `graphics::swap_frames();` → `if (!headless) graphics::swap_frames(); else graphics::flush_commands();` — check graphics.h: if `flush_commands` is internal-only, expose the existing public equivalent (M2a made readback flush implicitly; the histogram capture each frame already forces a flush — in that case `else {}` is fine; verify by running).
- Force `compute_histogram = true;` when headless (it defaults true anyway; assert it).

Energy verdict — around the statistics block, add:

```cpp
    // Headless acceptance: mean trace energy at data points must rise.
    static float e_first = -1.0f;
    static float e_last = 0.0f;
    if (compute_histogram) {
        if (e_first < 0.0f && simulation_config.n_iteration >= 10) e_first = mean;  // skip warmup
        e_last = mean;
        if (headless && simulation_config.n_iteration % 50 == 0)
            printf("[headless] iteration %d  E = %f\n", simulation_config.n_iteration, mean);
    }
```

and after the loop, before cleanup:

```cpp
    if (headless) {
        printf("[headless] E first=%f last=%f -> %s\n", e_first, e_last,
               (e_last > e_first * 1.05f) ? "ENERGY RISING" : "ENERGY NOT RISING");
        if (!(e_last > e_first * 1.05f)) { return 1; }
    }
```

(`mean` is computed inside the statistics scope — hoist a `float energy_mean_this_frame` out of it so the verdict code can see it; keep the in-scope math untouched.)

- [ ] **Step 2: Dataset generator**

`tests/gen_test_dataset.cpp` — writes a 400-point clustered synthetic catalog (three Gaussian blobs on a diagonal — enough structure for agents to converge on):

```cpp
// Writes config.polyp + <name>_metadata.txt + <name>.bin into argv[1],
// in the exact format main.cpp's loaders expect (4 floats per point: x y z mass).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

static unsigned s = 12345u;
static float frand() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / 16777216.0f; }
static float gauss() { return (frand() + frand() + frand() + frand() - 2.0f) * 0.8f; }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: gen_test_dataset <dir>\n"); return 2; }
    std::string dir(argv[1]);
    const int N = 400;
    const char *name = "testdata";

    FILE *cfg = fopen((dir + "/config.polyp").c_str(), "w");
    if (!cfg) { printf("cannot write to %s\n", dir.c_str()); return 2; }
    fprintf(cfg, "NUM_AGENTS=100000\nGRID_RESOLUTION=64\nGRID_PADDING=0.25\nSCREEN_X=640\nSCREEN_Y=480\nCAMERA_FOV=45\nHISTOGRAM_BASE=2.0\n");
    fclose(cfg);

    float centers[3][3] = {{-30, -30, -30}, {0, 0, 0}, {30, 30, 30}};
    FILE *bin = fopen((dir + "/" + name + ".bin").c_str(), "wb");
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    double mass_sum = 0.0;
    for (int i = 0; i < N; ++i) {
        float *c = centers[i % 3];
        float p[4] = { c[0] + 8.0f * gauss(), c[1] + 8.0f * gauss(), c[2] + 8.0f * gauss(),
                       1.0f + 99.0f * frand() };  // mass
        for (int k = 0; k < 3; ++k) { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
        mass_sum += p[3];
        fwrite(p, sizeof(float), 4, bin);
    }
    fclose(bin);

    // mean_weight matches main.cpp's log10(1+mass) weighting convention:
    // metadata's value is used as the normalizer, upstream files store the mean of the raw column.
    FILE *meta = fopen((dir + "/" + name + "_metadata.txt").c_str(), "w");
    fprintf(meta, "n=%d\nxmin=%f\nxmax=%f\nymin=%f\nymax=%f\nzmin=%f\nzmax=%f\nmean_weight=%f\n",
            N, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], (float)(mass_sum / N));
    fclose(meta);
    printf("wrote %d points to %s\n", N, dir.c_str());
    return 0;
}
```

Check main.cpp's metadata reader (`std::getline(file, varname, '=')` then `>>`) tolerates these key names — it ignores the names entirely (reads by position), so any `key=value` lines in the right ORDER work. Order above matches lines 377-384.

The dataset filename is compiled into main.cpp (`DATASET_NAME`). Add a second CLI flag in Step 1: `--dataset <path>` overriding the `filename` string (default stays `DATASET_NAME`), so the test can point at the generated files:

```cpp
        if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) dataset_override = argv[i + 1];
```

and `std::string filename(dataset_override ? dataset_override : DATASET_NAME);`.

- [ ] **Step 3: CMake — generator + smoke test**

```cmake
add_executable(gen_test_dataset tests/gen_test_dataset.cpp)

add_test(NAME energy_smoke
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_test_dataset>
    -DAPP=$<TARGET_FILE:polyphorm>
    -P ${CMAKE_SOURCE_DIR}/tests/run_energy_smoke.cmake)
set_tests_properties(energy_smoke PROPERTIES TIMEOUT 300)
```

`tests/run_energy_smoke.cmake`:

```cmake
# Generates a synthetic dataset in a fresh dir and runs the sim headless.
# polyphorm exits nonzero if energy did not rise — that IS the assertion.
set(WORK ${CMAKE_CURRENT_BINARY_DIR}/energy_smoke_work)
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})
execute_process(COMMAND ${GEN} ${WORK} RESULT_VARIABLE r1)
if(NOT r1 EQUAL 0)
  message(FATAL_ERROR "dataset generator failed: ${r1}")
endif()
execute_process(COMMAND ${APP} --headless 400 --dataset ${WORK}/testdata
                WORKING_DIRECTORY ${WORK} RESULT_VARIABLE r2)
if(NOT r2 EQUAL 0)
  message(FATAL_ERROR "energy smoke failed (exit ${r2}) — energy not rising or startup failure")
endif()
```

(main.cpp reads `config.polyp` from CWD — the WORKING_DIRECTORY handles that. `shaders/` paths are relative too: make the shader loads in Task 5 robust by trying `shaders/...` then `${repo}/shaders/...`? NO — keep it simple and correct: pass the shader dir the same way the tests do. Change Task 5's `load_compute` paths to be prefixed by a `SHADER_ROOT` compile definition: `target_compile_definitions(polyphorm PRIVATE SHADER_ROOT="${CMAKE_SOURCE_DIR}/shaders")` and `load_compute(SHADER_ROOT "/cs_agents_propagate.wgsl")`. Absolute path, works from any CWD, matches the test targets' pattern. Upstream's edit-and-reload workflow is preserved since the files are the repo's own shaders/.)

Apply that `SHADER_ROOT` change to Task 5's step 4 code while implementing (single source of truth: this note).

- [ ] **Step 4: Run the smoke**

```bash
nice -n 19 cmake --build build -j 8 && ctest --test-dir build -R energy_smoke --output-on-failure
```

Expected: `[headless] iteration ...  E = ...` lines with E increasing, exit 0, test PASS in well under the 300 s timeout (64³ grid, 100k agents, 400 iterations ≈ seconds on the M1 Max). If E does not rise: debug the SIM, do not loosen the 1.05 factor — likely suspects are the is_a ping-pong orientation (decay must read the texture agents just WROTE, translation-notes "open questions"), a missed uniform rebind, or wrong bind slots. The Task 4 micro-tests passing localizes the fault to main.cpp wiring.

- [ ] **Step 5: Full suite + windowed sanity**

`ctest --test-dir build` — all suites green. Then a 10-second windowed run from a dataset dir (`cd <workdir> && <repo>/build/polyphorm`): window opens, title shows pass counter climbing, Esc quits cleanly. Report the observed E trajectory.

- [ ] **Step 6: Commit**

```bash
git add main.cpp tests/gen_test_dataset.cpp tests/run_energy_smoke.cmake CMakeLists.txt
git commit -m "sim: headless mode + synthetic dataset + energy-rising acceptance test"
```

---

## Deferred / explicitly out of scope

- Particle & volume rendering, ImGui (M3/M4). `cs_agents_sort` port (upstream default-off, UI toggle commented out) — M3 at earliest, or drop.
- Framebuffer-vs-logical display sizing (M3, with the real blit).
- Half→f32 texture upload conversion: not needed in M2b (sim textures are created empty); becomes relevant only if a milestone uploads f16 texel data.
- Perf of 256 queue-submits/frame in the sort loop: dormant while sort is off.
- The `g_clear2d_f` (RGBA32_FLOAT 2D) clear kernel remains untested (accepted M2a gap).
- Window resizability (M1 carryover): stays `GLFW_RESIZABLE = FALSE` through M2b — nothing renders yet. If M3/M4 restore it, surface-reconfigure-on-resize belongs in `graphics::`.
