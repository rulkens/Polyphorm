# M2a: WebGPU graphics:: Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild `cpplib/graphics.{h,cpp}` and `cpplib/file_system.cpp` over WebGPU/Dawn so the compute path (buffers, 3D storage textures, uniform buffers, WGSL compute pipelines, slot-based binding, dispatch, synchronous readback) is fully functional and unit-tested headlessly — the foundation M2b's shader ports and main.cpp integration build on.

**Architecture:** The fork's `graphics::` keeps the exact function names/signatures main.cpp calls (inventoried in `docs/superpowers/research/m2/graphics-api-inventory.md`) but replaces D3D11 internals with WebGPU. Slot-based immediate-mode binding is preserved via shadow state: `set_*` calls record bindings; `run_compute()` assembles a fresh bind group from them against the pipeline's reflected layout. Binding convention (from the WGSL research): `@group(0) @binding(0)` = the constant buffer bound via `set_constant_buffer(cb, 0)`; `@group(1) @binding(N)` = resource at slot N (`set_texture_compute`/`set_structured_buffer`/`set_texture_sampled_compute` share one slot space, mirroring HLSL `u`/`t`/`s` registers into one group — see Task 5 notes). Readback stays synchronous (blocking `mapAsync` + ProcessEvents pump), faithfully reproducing D3D11 `Map(D3D11_MAP_READ)`'s same-frame stall. Render-side functions exist as inert-but-valid stubs (M3 makes them real). `gpu_context` splits into device-init and surface-init so tests run headless, and gains the device-limit gates the shader research proved necessary.

**Tech Stack:** C++17, CMake, Dawn (pinned `v20260807.193620`, already built in `build/`), WGSL, CTest.

## Global Constraints

- Complete fork: deletions are permanent, no upstream-compat shims. The ~20 unused `graphics::` API items (inventory §3) are deleted, not ported.
- Function names and signatures for the 39 used `graphics::` functions (inventory §1) are preserved except: D3D-specific parameter types (`DXGI_FORMAT`, `D3D11_FILTER`, `D3D11_PRIMITIVE_TOPOLOGY`, `LUID*`) become fork-owned enums (defined in Task 3); main.cpp's call sites are updated in M2b, not here.
- Device init MUST fatal-error naming the feature/limit if `float32-filterable` is unavailable or a required limit cannot be met (spec: Error handling).
- Every commit leaves `cmake --build build` green (incremental). NEVER wipe `build/` or clean-configure — Dawn re-downloads ~1.4 GB on the user's slow connection. Build with `nice -n 19 cmake --build build -j 8` (never bare `-j`).
- All tests must pass headlessly (no window) — GPU tests acquire a device without a surface.
- WGSL binding convention: `@group(0) @binding(0)` uniform config; `@group(1) @binding(N)` for slot-N resources. This is load-bearing for M2b's shaders — do not deviate.
- Commit messages end with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## File Structure

- `cpplib/file_system.cpp` — rewritten POSIX (header `file_system.h` unchanged: `File {void* data; size_t size;}`, `read_file`, `write_file`, `release_file`).
- `cpplib/gpu/gpu_context.h/.cpp` — split `gpu::init` into `gpu::init_device(GpuContext*)` (instance/adapter/device/queue + feature/limit gates, headless-safe) and `gpu::init_surface(GpuContext*, Window*)` (surface config at framebuffer size). `gpu::init(Window*)` remains as the composition of both (main_m1.cpp keeps compiling). New `GpuContext` fields for granted limits.
- `cpplib/graphics.h` — rewritten fork header: WebGPU-backed types, used-surface-only API, fork-owned enums.
- `cpplib/graphics.cpp` — WebGPU implementation (~700 lines), including one embedded builtin WGSL clear kernel.
- `shaders/tests/` — two tiny WGSL kernels used only by unit tests.
- `tests/file_system_tests.cpp`, `tests/graphics_tests.cpp` — new CTest targets (`cpplib_tests` stays as-is).

---

### Task 1: POSIX file_system port

**Files:**
- Modify: `cpplib/file_system.cpp` (full rewrite, Win32 → POSIX)
- Test: `tests/file_system_tests.cpp`
- Modify: `CMakeLists.txt` (uncomment `cpplib/file_system.cpp`, add test target)

**Interfaces:**
- Consumes: `cpplib/file_system.h` (unchanged: `struct File { void *data; size_t size; };`, `File read_file(const char *path);`, `void write_file(const char *path, void *data, size_t size);`, `void release_file(File file);` in `namespace file_system`)
- Produces: working `file_system::read_file`/`release_file` used by Task 5's shader loading and M2b's dataset load.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/file_system_tests.cpp
#include "../cpplib/file_system.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

int main() {
    // Round-trip: write a file, read it back, byte-identical.
    const char payload[] = "polyphorm\x00\x01\xffbinary";
    const size_t payload_size = sizeof(payload); // includes NUL + binary bytes
    const char *path = "fs_test_roundtrip.bin";

    file_system::write_file(path, (void *)payload, payload_size);
    file_system::File f = file_system::read_file(path);
    assert(f.data != nullptr);
    assert(f.size == payload_size);
    assert(memcmp(f.data, payload, payload_size) == 0);
    file_system::release_file(f);
    remove(path);

    // Missing file: data == nullptr, size == 0 (matches Win32 version's
    // failure contract: main.cpp checks file.data before use).
    file_system::File missing = file_system::read_file("no_such_file_xyz.bin");
    assert(missing.data == nullptr);
    assert(missing.size == 0);

    printf("file_system_tests: all passed\n");
    return 0;
}
```

- [ ] **Step 2: Add the test target and uncomment the source, run test to verify it fails**

In `CMakeLists.txt`: uncomment `cpplib/file_system.cpp` in the `polyphorm` target sources (drop its `# enabled in M2` marker), and add:

```cmake
add_executable(file_system_tests tests/file_system_tests.cpp cpplib/file_system.cpp)
target_compile_features(file_system_tests PRIVATE cxx_std_17)
add_test(NAME file_system_tests COMMAND file_system_tests)
```

Run: `cmake -B build && nice -n 19 cmake --build build -j 8`
Expected: FAIL — `file_system.cpp` still contains `#include <windows.h>` and does not compile on macOS. That compile error is this task's "failing test".

- [ ] **Step 3: Rewrite file_system.cpp**

```cpp
// cpplib/file_system.cpp
#include "file_system.h"

#include <cstdio>
#include <cstdlib>

namespace file_system {

File read_file(const char *path) {
    File file = {};
    FILE *fp = fopen(path, "rb");
    if (!fp) return file;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return file; }
    fseek(fp, 0, SEEK_SET);
    // calloc: original used HEAP_ZERO_MEMORY; shader loaders rely on the
    // buffer being usable as a NUL-terminated string when they pass
    // (char*)data with a separate length, so keep one spare zero byte.
    void *data = calloc(1, (size_t)size + 1);
    if (!data) { fclose(fp); return file; }
    size_t bytes_read = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    if (bytes_read != (size_t)size) { free(data); return file; }
    file.data = data;
    file.size = (size_t)size;
    return file;
}

void write_file(const char *path, void *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(data, 1, size, fp);
    fclose(fp);
}

void release_file(File file) {
    if (file.data) free(file.data);
}

}
```

NOTE: check `cpplib/file_system.h` before writing — if the actual signatures differ from the ones above (e.g. `std::string` paths, different member names, a bool return on `write_file`), the header wins: keep the header byte-identical and adapt the .cpp to it. Do the same for the test.

- [ ] **Step 4: Build and run the test**

Run: `cmake -B build && nice -n 19 cmake --build build -j 8 && ctest --test-dir build -R file_system_tests`
Expected: PASS (and the existing `cpplib_tests` still passes: run `ctest --test-dir build` with no filter).

- [ ] **Step 5: Commit**

```bash
git add cpplib/file_system.cpp tests/file_system_tests.cpp CMakeLists.txt
git commit -m "port: file_system Win32 -> POSIX, same public API"
```

---

### Task 2: gpu_context split + device limit gates

**Files:**
- Modify: `cpplib/gpu/gpu_context.h`
- Modify: `cpplib/gpu/gpu_context.cpp`

**Interfaces:**
- Consumes: existing `GpuContext`, `gpu::init(Window*)`, `gpu::clear_and_present` (M1, commit 7eb715f + f7c2296).
- Produces: `bool gpu::init_device(GpuContext *ctx)` (headless-safe; performs ALL feature/limit gating), `bool gpu::init_surface(GpuContext *ctx, Window *window)` (surface config at framebuffer resolution — preserve the M1 Retina fix), `gpu::init(Window*)` = both in sequence (signature unchanged). New `GpuContext` fields: `uint32_t max_workgroup_invocations; uint64_t max_storage_buffer_binding_size; uint64_t max_buffer_size;`. Task 3's `graphics::init`/`init_swap_chain` call these; unit tests call `init_device` only.

**Why the gates (from `docs/superpowers/research/m2/wgsl-drafts/translation-notes.md` §5 and `m1-carryovers.md`):** the ported shaders need workgroups of 1000 (`cs_agents_propagate`) and 512 (`cs_field_decay`) invocations vs the WebGPU default limit of 256; grid readback (M5) needs buffers far beyond the 256 MiB / 128 MiB defaults. Metal on Apple Silicon supports 1024 invocations — request it, but degrade gracefully: M2b's shaders take `WG_X/Y/Z` override constants, so if the adapter grants < 1000 the sim reshapes workgroups instead of dying. Buffer limits: request the adapter's own maximums (clamped requests are an error in WebGPU; requesting exactly what the adapter reports is always valid).

- [ ] **Step 1: Restructure gpu_context**

In `gpu_context.h`, add to `GpuContext`:

```cpp
    uint32_t max_workgroup_invocations = 0;      // granted maxComputeInvocationsPerWorkgroup
    uint64_t max_storage_buffer_binding_size = 0;
    uint64_t max_buffer_size = 0;
```

and declare:

```cpp
bool init_device(GpuContext *ctx);                 // instance/adapter/device/queue; headless-safe
bool init_surface(GpuContext *ctx, Window *window); // surface create+configure (framebuffer size)
bool init(GpuContext *ctx, Window *window);        // both, in order (existing signature preserved)
```

(Keep whatever the existing `init` signature is — read the header first; if M1's is `GpuContext init(Window*)` returning by value, preserve that shape for `init` and make `init_device`/`init_surface` match its style. The M1 code is the source of truth for style; the split and the new fields are the requirement.)

In `gpu_context.cpp`, move the existing adapter/device request (including the `float32-filterable` fatal gate, the error/lost callbacks, and the ProcessEvents pumps) into `init_device`, and the surface creation/configure (including the `glfwGetFramebufferSize` Retina logic) into `init_surface`. Then add the limit logic to `init_device`, after adapter acquisition, before device request:

```cpp
    // Adapter-supported limits: request what the sim needs, degrade where allowed.
    wgpu::Limits supported = {};
    adapter.GetLimits(&supported);

    wgpu::Limits required = {};   // zero-init: fields we don't set stay at spec defaults
    // Compute workgroup: shaders prefer their original 1000/512-invocation
    // shapes; they can reshape via override constants if the adapter grants
    // less, so request the adapter's own maximum rather than gating hard.
    required.maxComputeInvocationsPerWorkgroup = supported.maxComputeInvocationsPerWorkgroup;
    required.maxComputeWorkgroupSizeX = supported.maxComputeWorkgroupSizeX;
    required.maxComputeWorkgroupSizeY = supported.maxComputeWorkgroupSizeY;
    required.maxComputeWorkgroupSizeZ = supported.maxComputeWorkgroupSizeZ;
    // Storage/readback: VAC-scale grids need multi-GB buffers (M5 export
    // readback of a 712x1200x728 r32float trace is ~2.5 GB).
    required.maxBufferSize = supported.maxBufferSize;
    required.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
    required.maxTextureDimension3D = supported.maxTextureDimension3D;

    // Hard floors: below these the sim cannot run at all; fatal NAMES the limit.
    if (supported.maxComputeInvocationsPerWorkgroup < 256)
        fatal("adapter limit too low: maxComputeInvocationsPerWorkgroup < 256");
    if (supported.maxTextureDimension3D < 1024)
        fatal("adapter limit too low: maxTextureDimension3D < 1024 (grid resolution)");
```

Wire `required` into the `wgpu::DeviceDescriptor` (`requiredLimits`), and after device creation store the granted values:

```cpp
    wgpu::Limits granted = {};
    ctx.device.GetLimits(&granted);
    ctx.max_workgroup_invocations = granted.maxComputeInvocationsPerWorkgroup;
    ctx.max_storage_buffer_binding_size = granted.maxStorageBufferBindingSize;
    ctx.max_buffer_size = granted.maxBufferSize;
```

API-adaptation note (same rule as M1 Task 4): if the pinned Dawn's `webgpu_cpp.h` names differ (`wgpu::Limits` vs `wgpu::SupportedLimits`/`RequiredLimits` wrappers with a `.limits` member — Dawn has had both shapes), adapt to the actual generated header at `build/_deps/dawn-build/gen/include/dawn/webgpu_cpp.h` and document the adaptation in your report. The behavior (request adapter maxima for the six limits above; two named fatal floors; store three granted values) is the contract.

- [ ] **Step 2: Build + run**

Run: `cmake -B build && nice -n 19 cmake --build build -j 8 && ctest --test-dir build`
Expected: all green. Then run `./build/polyphorm` for ~3 seconds (backgrounded, kill after) — window still opens, pulsing clear still works, no device-creation errors on stderr (proves the limit request is valid on the real adapter).

- [ ] **Step 3: Print granted limits once at startup**

In `init_device`, after storing granted values, log one line (stderr or the existing logging style used in gpu_context.cpp):

```cpp
    fprintf(stderr, "[gpu] limits: workgroup_invocations=%u storage_binding=%llu MiB buffer=%llu MiB\n",
            ctx.max_workgroup_invocations,
            (unsigned long long)(ctx.max_storage_buffer_binding_size >> 20),
            (unsigned long long)(ctx.max_buffer_size >> 20));
```

Rebuild, run once, and record the actual granted numbers in your report — M2b's plan needs to know whether `maxComputeInvocationsPerWorkgroup >= 1000` holds on this machine (expected: 1024 on Apple Silicon Metal).

- [ ] **Step 4: Commit**

```bash
git add cpplib/gpu/gpu_context.h cpplib/gpu/gpu_context.cpp
git commit -m "gpu: split device/surface init, request adapter-max limits with named fatal floors"
```

---

### Task 3: graphics.h fork header + context/frame lifecycle

**Files:**
- Modify: `cpplib/graphics.h` (full rewrite)
- Modify: `cpplib/graphics.cpp` (full rewrite — this task implements init/frame/present/release; Tasks 4-5 fill in resources/compute in the same file)
- Modify: `CMakeLists.txt` (uncomment `cpplib/graphics.cpp`... if it is not among the commented lines, add it to the `polyphorm` sources)

**Interfaces:**
- Consumes: `gpu::init_device`, `gpu::init_surface`, `GpuContext` fields (Task 2).
- Produces: the complete fork `graphics.h` below — Tasks 4-6 implement the rest of its declarations; M2b compiles main.cpp against it. The internal `Frame state` static block and `flush_commands()` helper (defined here) are used by Tasks 4-5.

- [ ] **Step 1: Write the fork graphics.h**

The complete header. Every function main.cpp calls (inventory §1) survives with its name; D3D types are replaced; unused API (inventory §3) is gone. `// M2a: stub` marks functions implemented as inert stubs until M3/M5.

```cpp
#pragma once
#include <stdint.h>
#include <string>
#include <webgpu/webgpu_cpp.h>

#include "gpu/gpu_context.h"

struct Window;

namespace graphics {

// ---- fork-owned enums (replace DXGI/D3D11 enums) ----
enum class Format {
    UNKNOWN,
    R32_FLOAT,        // replaces DXGI_FORMAT_R16_FLOAT for storage textures:
                      // WebGPU's only read_write float storage format. Exact
                      // behavioural match in REGIME_SDSS (research notes §0).
    RGBA32_FLOAT,     // display_tex
    R32_UINT,         // display_tex_uint (atomic splat target)
    RGBA8_UNORM,
    RGBA8_UNORM_SRGB, // window view format (preserves the sRGB-over-UNORM quirk)
};
enum SampleMode { CLAMP, WRAP, BORDER };
enum class Filter { POINT, LINEAR, ANISOTROPIC };
enum class BlendType { OPAQUE, ALPHA };
enum class Topology { TRIANGLELIST, TRIANGLESTRIP };

// ---- types ----
struct GraphicsContext {
    wgpu::Device device;
    wgpu::Queue queue;
    GpuContext *gpu = nullptr;
};

struct RenderTarget {
    wgpu::Texture texture;       // null when is_window (acquired per frame)
    wgpu::TextureView rt_view;   // null when is_window
    uint32_t width = 0, height = 0;
    bool is_window = false;
};

struct DepthBuffer {             // created by main.cpp but never bound (inventory §4)
    wgpu::Texture texture;
    wgpu::TextureView ds_view;
    uint32_t width = 0, height = 0;
};

struct Texture2D {
    wgpu::Texture texture;
    wgpu::TextureView sr_view;   // sampled view
    wgpu::TextureView ua_view;   // storage view (name kept from D3D11 for familiarity)
    uint32_t width = 0, height = 0;
    Format format = Format::UNKNOWN;
};

struct Texture3D {
    wgpu::Texture texture;
    wgpu::TextureView sr_view;
    wgpu::TextureView ua_view;
    uint32_t width = 0, height = 0, depth = 0;
    Format format = Format::UNKNOWN;
};

struct TextureSampler { wgpu::Sampler sampler; };

struct Mesh {
    wgpu::Buffer vertex_buffer;
    uint32_t vertex_stride = 0, vertex_offset = 0, vertex_count = 0;
    Topology topology = Topology::TRIANGLELIST;
};

struct ConstantBuffer { wgpu::Buffer buffer; uint32_t size = 0; };

struct StructuredBuffer {
    wgpu::Buffer buffer;
    wgpu::Buffer readback;       // lazily created by capture_structured_buffer
    uint32_t element_stride = 0, num_elements = 0, size = 0;
};

struct VertexShader  { wgpu::ShaderModule module; bool valid = false; }; // M2a: stub module
struct PixelShader   { wgpu::ShaderModule module; bool valid = false; }; // M2a: stub module
struct ComputeShader { wgpu::ComputePipeline pipeline; bool valid = false; };

// Named override constants passed at compute-pipeline creation (quirk toggles,
// WG_X/Y/Z workgroup sizes — research notes §1).
struct ShaderConstant { const char *name; double value; };

// ---- lifecycle ----
bool init();                            // device only (headless-safe); sets graphics_context
bool init_swap_chain(Window *window);   // surface config (framebuffer size)
void swap_frames();                     // submit + present
void release();                         // teardown, last call

// ---- render targets / clears ----
RenderTarget get_render_target_window();
DepthBuffer get_depth_buffer(uint32_t width, uint32_t height);
void set_render_targets_viewport(RenderTarget *buffer);
void clear_render_target(RenderTarget *buffer, float r, float g, float b, float a);

// ---- textures ----
Texture2D get_texture2D(void *data, uint32_t width, uint32_t height, Format format,
                        uint32_t pixel_byte_count = 4);
Texture3D get_texture3D(void *data, uint32_t width, uint32_t height, uint32_t depth,
                        Format format, uint32_t pixel_byte_count = 4);
Texture2D load_texture2D(std::string filename);                 // M2a: stub (1x1 white)
void save_texture3D(Texture3D *texture, std::string filename);  // M2a: stub (M5)
void save_texture2D_HDR(Texture2D *texture, std::string filename); // M2a: stub (M5)
uint32_t capture_current_frame();                               // M2a: stub (M5)

// Replaces main.cpp's raw ClearUnorderedAccessView* on .ua_view (inventory §2):
void clear_texture(Texture3D *texture, float value);
void clear_texture(Texture2D *texture, float value);
void clear_texture_uint(Texture2D *texture, uint32_t value);

// ---- binding: render stage (M2a: stubs, real in M3) ----
void set_texture(Texture2D *texture, uint32_t slot);
void set_texture(Texture3D *texture, uint32_t slot);
void unset_texture(uint32_t slot);
void set_texture_sampler(TextureSampler *sampler, uint32_t slot);

// ---- binding: compute stage (real) ----
void set_texture_compute(Texture2D *texture, uint32_t slot);          // storage view
void set_texture_compute(Texture3D *texture, uint32_t slot);
void set_texture_sampled_compute(Texture2D *texture, uint32_t slot);  // sampled view
void set_texture_sampled_compute(Texture3D *texture, uint32_t slot);
void set_texture_sampler_compute(TextureSampler *sampler, uint32_t slot);
void unset_texture_compute(uint32_t slot);
void unset_texture_sampled_compute(uint32_t slot);

// ---- samplers ----
TextureSampler get_texture_sampler(SampleMode mode = CLAMP, Filter filter = Filter::POINT);

// ---- blend ----
void set_blend_state(BlendType type);   // M2a: recorded, consumed by M3 draws

// ---- mesh / draw ----
Mesh get_mesh(void *vertices, uint32_t vertex_count, uint32_t vertex_stride,
              void *indices, uint32_t index_count, uint32_t index_byte_size,
              Topology topology = Topology::TRIANGLELIST);
void draw_mesh(Mesh *mesh);             // M2a: stub (warn once)

// ---- constant buffers ----
ConstantBuffer get_constant_buffer(uint32_t size);
void update_constant_buffer(ConstantBuffer *buffer, void *data);
void set_constant_buffer(ConstantBuffer *buffer, uint32_t slot); // slot must be 0 (group 0 binding 0)

// ---- structured buffers ----
StructuredBuffer get_structured_buffer(int element_stride, int num_elements);
void update_structured_buffer(StructuredBuffer *buffer, void *data);
void set_structured_buffer(StructuredBuffer *buffer, uint32_t slot);
void capture_structured_buffer(StructuredBuffer *buffer, void *mapped_data,
                               uint32_t num_elements, size_t element_size);

// ---- shaders ----
VertexShader get_vertex_shader_from_code(char *code, uint32_t code_length);   // M2a: stub
PixelShader get_pixel_shader_from_code(char *code, uint32_t code_length);     // M2a: stub
ComputeShader get_compute_shader_from_code(char *code, uint32_t code_length,
                                           const ShaderConstant *constants = nullptr,
                                           uint32_t constant_count = 0);
void set_vertex_shader(VertexShader *shader);   // M2a: stub
void set_pixel_shader(PixelShader *shader);     // M2a: stub
void set_compute_shader(ComputeShader *shader);

// ---- dispatch ----
void run_compute(int group_count_x, int group_count_y, int group_count_z);

// ---- is_ready (assert guards in main.cpp) ----
bool is_ready(RenderTarget *ptr);
bool is_ready(DepthBuffer *ptr);
bool is_ready(Texture2D *ptr);
bool is_ready(Texture3D *ptr);
bool is_ready(Mesh *ptr);
bool is_ready(ConstantBuffer *ptr);
bool is_ready(StructuredBuffer *ptr);
bool is_ready(TextureSampler *ptr);
bool is_ready(VertexShader *ptr);
bool is_ready(PixelShader *ptr);
bool is_ready(ComputeShader *ptr);

// ---- release overloads (fork keeps only the ones main.cpp calls) ----
void release(RenderTarget *ptr);
void release(DepthBuffer *ptr);
void release(Texture2D *ptr);
void release(Texture3D *ptr);
void release(Mesh *ptr);
void release(ConstantBuffer *ptr);
void release(StructuredBuffer *ptr);
void release(TextureSampler *ptr);
void release(VertexShader *ptr);
void release(PixelShader *ptr);
void release(ComputeShader *ptr);

}

extern graphics::GraphicsContext *graphics_context;
```

- [ ] **Step 2: Implement lifecycle + frame model in graphics.cpp**

Start the new `graphics.cpp` with the internal state and the lifecycle functions. The frame model: one `wgpu::CommandEncoder` accumulates work; `flush_commands()` finishes+submits it and starts a new one; `swap_frames()` flushes and presents; readback (Task 5) flushes before copying. Clears are standalone render passes (LoadOp::Clear, StoreOp::Store, no draws) — faithful to D3D11's free-standing `ClearRenderTargetView`.

```cpp
#include "graphics.h"
#include "logging.h"   // check what M1 code uses; if logging.cpp is not yet
                       // compiled, use fprintf(stderr, ...) like gpu_context.cpp
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

graphics::GraphicsContext *graphics_context = nullptr;

namespace graphics {

// ---- internal state ----
static GraphicsContext g_ctx;
static GpuContext g_gpu;
static wgpu::CommandEncoder g_encoder;
static wgpu::SurfaceTexture g_surface_tex;       // acquired lazily per frame
static bool g_surface_tex_acquired = false;
static BlendType g_blend = BlendType::OPAQUE;

// Compute binding shadow state (Task 5 consumes):
struct BoundSlot {
    enum class Kind { NONE, STORAGE_BUFFER, STORAGE_TEX, SAMPLED_TEX, SAMPLER } kind = Kind::NONE;
    wgpu::Buffer buffer;
    uint64_t buffer_size = 0;
    wgpu::TextureView view;
    wgpu::Sampler sampler;
};
static const uint32_t MAX_SLOTS = 16;
static BoundSlot g_compute_slots[MAX_SLOTS];
static wgpu::Buffer g_uniform_buffer;            // group 0 binding 0
static uint64_t g_uniform_size = 0;
static ComputeShader *g_compute_shader = nullptr;

static void ensure_encoder() {
    if (!g_encoder) g_encoder = g_ctx.device.CreateCommandEncoder();
}

static void flush_commands() {
    if (!g_encoder) return;
    wgpu::CommandBuffer commands = g_encoder.Finish();
    g_ctx.queue.Submit(1, &commands);
    g_encoder = nullptr;
}

// Blocking pump: process events until `done` flips. Used by readback and
// pipeline-error scopes. Mirrors gpu_context.cpp's request pumps.
static void wait_for(bool *done) {
    while (!*done) g_gpu.instance.ProcessEvents();
}

bool init() {
    if (!gpu::init_device(&g_gpu)) return false;
    g_ctx.device = g_gpu.device;
    g_ctx.queue = g_gpu.queue;
    g_ctx.gpu = &g_gpu;
    graphics_context = &g_ctx;
    return true;
}

bool init_swap_chain(Window *window) {
    return gpu::init_surface(&g_gpu, window);
}

RenderTarget get_render_target_window() {
    RenderTarget rt = {};
    rt.is_window = true;
    rt.width = g_gpu.width;
    rt.height = g_gpu.height;
    return rt;
}

// Acquire the surface texture for this frame if not already held.
static wgpu::TextureView window_view() {
    if (!g_surface_tex_acquired) {
        g_gpu.surface.GetCurrentTexture(&g_surface_tex);
        if (g_surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
            g_surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            fprintf(stderr, "[graphics] surface texture acquisition failed\n");
            return nullptr;
        }
        g_surface_tex_acquired = true;
    }
    wgpu::TextureViewDescriptor view_desc = {};
    view_desc.format = wgpu::TextureFormat::BGRA8UnormSrgb; // sRGB view over the
    // BGRA8Unorm surface — preserves the original's sRGB-over-UNORM gamma quirk
    // (inventory §1, get_render_target_window). Surface must be configured with
    // this viewFormat in gpu::init_surface — if M1's configure lacks
    // `viewFormats`, add it there (1-line: viewFormatCount=1, viewFormats=&srgb).
    return g_surface_tex.texture.CreateView(&view_desc);
}

void set_render_targets_viewport(RenderTarget *buffer) {
    // D3D11 version set OM targets + viewport. WebGPU render passes carry the
    // target; viewport is full-target by default. Nothing to record until a
    // clear or (M3) a draw — this call is intentionally a no-op that validates
    // the argument shape.
    (void)buffer;
}

void clear_render_target(RenderTarget *buffer, float r, float g, float b, float a) {
    ensure_encoder();
    wgpu::TextureView view = buffer->is_window ? window_view() : buffer->rt_view;
    if (!view) return;
    wgpu::RenderPassColorAttachment att = {};
    att.view = view;
    att.loadOp = wgpu::LoadOp::Clear;
    att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {r, g, b, a};
    wgpu::RenderPassDescriptor pass_desc = {};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &att;
    wgpu::RenderPassEncoder pass = g_encoder.BeginRenderPass(&pass_desc);
    pass.End();
}

void swap_frames() {
    flush_commands();
    if (g_surface_tex_acquired) {
        g_gpu.surface.Present();
        g_surface_tex = {};
        g_surface_tex_acquired = false;
    }
}

void release() {
    flush_commands();
    g_uniform_buffer = nullptr;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) g_compute_slots[i] = {};
    graphics_context = nullptr;
    // wgpu C++ handles are refcounted; dropping them tears down the device.
    g_ctx = {};
    g_gpu = {};
}

}
```

API-adaptation rule (applies to this task and Tasks 4-6, same as M1 Task 4): if the pinned Dawn header names differ from the above (`SurfaceTexture.status` enumerators, `SurfaceGetCurrentTextureStatus` naming, `TextureViewDescriptor` fields), adapt to `build/_deps/dawn-build/gen/include/dawn/webgpu_cpp.h` — M1's `gpu_context.cpp` compiled against it and is the reference for working spellings. Behavior is the contract. Document adaptations in your report.

- [ ] **Step 3: Compile check**

Add `cpplib/graphics.cpp` to the `polyphorm` target sources. The remaining header declarations are not yet defined — that's fine for compiling `graphics.cpp` itself (they are declarations), but the target must still LINK, and `main_m1.cpp` doesn't call graphics functions, so the only object file with undefined references would be none. If the linker still complains (e.g. `-Wl,-undefined` strictness on the unimplemented declarations — it should not, since nothing references them), add temporary empty definitions at the bottom of graphics.cpp under a `// M2a Task 4/5/6 implement these` banner and remove each as later tasks land.

Run: `cmake -B build && nice -n 19 cmake --build build -j 8 && ctest --test-dir build`
Expected: builds green, all existing tests pass, `./build/polyphorm` (3-second run) still works.

- [ ] **Step 4: Commit**

```bash
git add cpplib/graphics.h cpplib/graphics.cpp CMakeLists.txt
git commit -m "graphics: fork header (WebGPU types, used-surface only) + context/frame lifecycle"
```

---

### Task 4: Resources — buffers, textures, samplers, clears

**Files:**
- Modify: `cpplib/graphics.cpp` (append implementations)

**Interfaces:**
- Consumes: Task 3's internal state (`g_ctx`, `g_encoder`, `ensure_encoder`, `flush_commands`, `g_compute_slots`).
- Produces: `get_constant_buffer`/`update_constant_buffer`, `get_structured_buffer`/`update_structured_buffer`, `get_texture2D`/`get_texture3D`, `get_texture_sampler`, `clear_texture`/`clear_texture_uint`, `get_depth_buffer`, all `is_ready`/`release` overloads for these types. Task 5 dispatches against these resources.

- [ ] **Step 1: Implement buffers and textures**

Append to `graphics.cpp`:

```cpp
namespace graphics {

static wgpu::TextureFormat to_wgpu(Format f) {
    switch (f) {
        case Format::R32_FLOAT:        return wgpu::TextureFormat::R32Float;
        case Format::RGBA32_FLOAT:     return wgpu::TextureFormat::RGBA32Float;
        case Format::R32_UINT:         return wgpu::TextureFormat::R32Uint;
        case Format::RGBA8_UNORM:      return wgpu::TextureFormat::RGBA8Unorm;
        case Format::RGBA8_UNORM_SRGB: return wgpu::TextureFormat::RGBA8UnormSrgb;
        default:                       return wgpu::TextureFormat::Undefined;
    }
}

static uint32_t bytes_per_pixel(Format f) {
    switch (f) {
        case Format::R32_FLOAT: case Format::R32_UINT: return 4;
        case Format::RGBA32_FLOAT: return 16;
        case Format::RGBA8_UNORM: case Format::RGBA8_UNORM_SRGB: return 4;
        default: return 0;
    }
}

ConstantBuffer get_constant_buffer(uint32_t size) {
    ConstantBuffer cb = {};
    wgpu::BufferDescriptor desc = {};
    desc.size = (size + 15u) & ~15u;   // round to 16: uniform binding size floor
    desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    cb.buffer = g_ctx.device.CreateBuffer(&desc);
    cb.size = size;
    return cb;
}

void update_constant_buffer(ConstantBuffer *buffer, void *data) {
    // Whole-buffer overwrite, like the D3D11 Map(WRITE_DISCARD)+memcpy.
    g_ctx.queue.WriteBuffer(buffer->buffer, 0, data, buffer->size);
}

void set_constant_buffer(ConstantBuffer *buffer, uint32_t slot) {
    // The original bound to all 4 stages at `slot`; main.cpp only ever uses
    // slot 0 (inventory §1). Fork contract: slot 0 == @group(0) @binding(0).
    assert(slot == 0 && "fork supports constant buffer slot 0 only");
    g_uniform_buffer = buffer->buffer;
    g_uniform_size = (buffer->size + 15u) & ~15u;
}

StructuredBuffer get_structured_buffer(int element_stride, int num_elements) {
    StructuredBuffer sb = {};
    wgpu::BufferDescriptor desc = {};
    desc.size = (uint64_t)element_stride * (uint64_t)num_elements;
    desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst |
                 wgpu::BufferUsage::CopySrc;   // CopySrc: readback path
    sb.buffer = g_ctx.device.CreateBuffer(&desc);
    sb.element_stride = (uint32_t)element_stride;
    sb.num_elements = (uint32_t)num_elements;
    sb.size = (uint32_t)desc.size;
    return sb;
}

void update_structured_buffer(StructuredBuffer *buffer, void *data) {
    g_ctx.queue.WriteBuffer(buffer->buffer, 0, data, buffer->size);
}

static wgpu::Texture make_texture(wgpu::TextureDimension dim, uint32_t w, uint32_t h,
                                  uint32_t d, Format format) {
    wgpu::TextureDescriptor desc = {};
    desc.dimension = dim;
    desc.size = {w, h, d};
    desc.format = to_wgpu(format);
    desc.mipLevelCount = 1;
    desc.usage = wgpu::TextureUsage::TextureBinding |     // sampled (sr_view)
                 wgpu::TextureUsage::StorageBinding |     // storage (ua_view)
                 wgpu::TextureUsage::CopySrc |            // export/readback
                 wgpu::TextureUsage::CopyDst;             // initial-data upload
    return g_ctx.device.CreateTexture(&desc);
}

Texture2D get_texture2D(void *data, uint32_t width, uint32_t height, Format format,
                        uint32_t pixel_byte_count) {
    (void)pixel_byte_count; // format determines the real size
    Texture2D t = {};
    t.texture = make_texture(wgpu::TextureDimension::e2D, width, height, 1, format);
    t.sr_view = t.texture.CreateView();
    t.ua_view = t.texture.CreateView();
    t.width = width; t.height = height; t.format = format;
    if (data) {
        wgpu::TexelCopyTextureInfo dst = {};
        dst.texture = t.texture;
        wgpu::TexelCopyBufferLayout layout = {};
        layout.bytesPerRow = width * bytes_per_pixel(format);
        layout.rowsPerImage = height;
        wgpu::Extent3D extent = {width, height, 1};
        g_ctx.queue.WriteTexture(&dst, data, (uint64_t)layout.bytesPerRow * height,
                                 &layout, &extent);
    }
    return t;
}

Texture3D get_texture3D(void *data, uint32_t width, uint32_t height, uint32_t depth,
                        Format format, uint32_t pixel_byte_count) {
    (void)pixel_byte_count;
    Texture3D t = {};
    t.texture = make_texture(wgpu::TextureDimension::e3D, width, height, depth, format);
    t.sr_view = t.texture.CreateView();
    t.ua_view = t.texture.CreateView();
    t.width = width; t.height = height; t.depth = depth; t.format = format;
    if (data) {
        wgpu::TexelCopyTextureInfo dst = {};
        dst.texture = t.texture;
        wgpu::TexelCopyBufferLayout layout = {};
        layout.bytesPerRow = width * bytes_per_pixel(format);
        layout.rowsPerImage = height;
        wgpu::Extent3D extent = {width, height, depth};
        g_ctx.queue.WriteTexture(&dst, data,
                                 (uint64_t)layout.bytesPerRow * height * depth,
                                 &layout, &extent);
    }
    // NOTE the original leaves data==NULL textures uninitialized; WebGPU
    // zero-initializes. That is a (beneficial) difference: the original relied
    // on main.cpp clearing before use anyway (inventory §2). Nothing to do.
    return t;
}

DepthBuffer get_depth_buffer(uint32_t width, uint32_t height) {
    // Created by main.cpp but never bound (inventory §4) — real texture, inert.
    DepthBuffer db = {};
    wgpu::TextureDescriptor desc = {};
    desc.size = {width, height, 1};
    desc.format = wgpu::TextureFormat::Depth24PlusStencil8;
    desc.usage = wgpu::TextureUsage::RenderAttachment;
    db.texture = g_ctx.device.CreateTexture(&desc);
    db.ds_view = db.texture.CreateView();
    db.width = width; db.height = height;
    return db;
}

TextureSampler get_texture_sampler(SampleMode mode, Filter filter) {
    TextureSampler s = {};
    wgpu::SamplerDescriptor desc = {};
    wgpu::AddressMode am = mode == WRAP ? wgpu::AddressMode::Repeat
                        : wgpu::AddressMode::ClampToEdge; // BORDER: WebGPU has no
    // border color sampler in core; CLAMP is the closest. main.cpp only uses
    // CLAMP (inventory §1), so this is unreachable-in-practice.
    desc.addressModeU = am; desc.addressModeV = am; desc.addressModeW = am;
    if (filter == Filter::POINT) {
        desc.magFilter = wgpu::FilterMode::Nearest;
        desc.minFilter = wgpu::FilterMode::Nearest;
        desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
    } else {
        desc.magFilter = wgpu::FilterMode::Linear;
        desc.minFilter = wgpu::FilterMode::Linear;
        desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        if (filter == Filter::ANISOTROPIC) desc.maxAnisotropy = 16;
    }
    s.sampler = g_ctx.device.CreateSampler(&desc);
    return s;
}

}
```

- [ ] **Step 2: Implement clear_texture via a builtin compute kernel**

WebGPU has no UAV-clear primitive; 3D textures cannot be render-pass attachments. One embedded WGSL kernel per dimensionality, compiled lazily on first use:

```cpp
namespace graphics {

static const char *CLEAR_TEX3D_WGSL = R"(
@group(0) @binding(0) var<uniform> clear_value : vec4<f32>;
@group(1) @binding(0) var target : texture_storage_3d<r32float, write>;
@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(target);
    if (gid.x >= dims.x || gid.y >= dims.y || gid.z >= dims.z) { return; }
    textureStore(target, gid, clear_value);
}
)";

static const char *CLEAR_TEX2D_F_WGSL = R"(
@group(0) @binding(0) var<uniform> clear_value : vec4<f32>;
@group(1) @binding(0) var target : texture_storage_2d<rgba32float, write>;
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(target);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }
    textureStore(target, gid.xy, clear_value);
}
)";

static const char *CLEAR_TEX2D_U_WGSL = R"(
@group(0) @binding(0) var<uniform> clear_value : vec4<u32>;
@group(1) @binding(0) var target : texture_storage_2d<r32uint, write>;
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(target);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }
    textureStore(target, gid.xy, clear_value);
}
)";

// One lazily-built pipeline + scratch uniform per clear kernel.
struct ClearKernel {
    wgpu::ComputePipeline pipeline;
    wgpu::Buffer uniform;   // 16 bytes
};
static ClearKernel g_clear3d, g_clear2d_f, g_clear2d_u;

static void ensure_clear_kernel(ClearKernel *k, const char *wgsl) {
    if (k->pipeline) return;
    wgpu::ShaderSourceWGSL src = {};
    src.code = wgsl;
    wgpu::ShaderModuleDescriptor mod_desc = {};
    mod_desc.nextInChain = &src;
    wgpu::ShaderModule module = g_ctx.device.CreateShaderModule(&mod_desc);
    wgpu::ComputePipelineDescriptor desc = {};
    desc.compute.module = module;
    k->pipeline = g_ctx.device.CreateComputePipeline(&desc);
    wgpu::BufferDescriptor bdesc = {};
    bdesc.size = 16;
    bdesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    k->uniform = g_ctx.device.CreateBuffer(&bdesc);
}

static void run_clear(ClearKernel *k, wgpu::TextureView view, const void *value16,
                      uint32_t gx, uint32_t gy, uint32_t gz) {
    g_ctx.queue.WriteBuffer(k->uniform, 0, value16, 16);
    wgpu::BindGroupEntry e0 = {};
    e0.binding = 0; e0.buffer = k->uniform; e0.size = 16;
    wgpu::BindGroupDescriptor g0 = {};
    g0.layout = k->pipeline.GetBindGroupLayout(0);
    g0.entryCount = 1; g0.entries = &e0;
    wgpu::BindGroup group0 = g_ctx.device.CreateBindGroup(&g0);
    wgpu::BindGroupEntry e1 = {};
    e1.binding = 0; e1.textureView = view;
    wgpu::BindGroupDescriptor g1 = {};
    g1.layout = k->pipeline.GetBindGroupLayout(1);
    g1.entryCount = 1; g1.entries = &e1;
    wgpu::BindGroup group1 = g_ctx.device.CreateBindGroup(&g1);

    ensure_encoder();
    wgpu::ComputePassEncoder pass = g_encoder.BeginComputePass();
    pass.SetPipeline(k->pipeline);
    pass.SetBindGroup(0, group0);
    pass.SetBindGroup(1, group1);
    pass.DispatchWorkgroups(gx, gy, gz);
    pass.End();
}

void clear_texture(Texture3D *texture, float value) {
    ensure_clear_kernel(&g_clear3d, CLEAR_TEX3D_WGSL);
    float v[4] = {value, value, value, value};
    run_clear(&g_clear3d, texture->ua_view, v,
              (texture->width + 3) / 4, (texture->height + 3) / 4,
              (texture->depth + 3) / 4);
}

void clear_texture(Texture2D *texture, float value) {
    assert(texture->format == Format::RGBA32_FLOAT);
    ensure_clear_kernel(&g_clear2d_f, CLEAR_TEX2D_F_WGSL);
    float v[4] = {value, value, value, value};
    run_clear(&g_clear2d_f, texture->ua_view, v,
              (texture->width + 7) / 8, (texture->height + 7) / 8, 1);
}

void clear_texture_uint(Texture2D *texture, uint32_t value) {
    assert(texture->format == Format::R32_UINT);
    ensure_clear_kernel(&g_clear2d_u, CLEAR_TEX2D_U_WGSL);
    uint32_t v[4] = {value, value, value, value};
    run_clear(&g_clear2d_u, texture->ua_view, v,
              (texture->width + 7) / 8, (texture->height + 7) / 8, 1);
}

}
```

- [ ] **Step 3: Implement is_ready + release overloads**

```cpp
namespace graphics {

bool is_ready(RenderTarget *p)     { return p->is_window || p->rt_view != nullptr; }
bool is_ready(DepthBuffer *p)      { return p->ds_view != nullptr; }
bool is_ready(Texture2D *p)        { return p->texture != nullptr; }
bool is_ready(Texture3D *p)        { return p->texture != nullptr; }
bool is_ready(Mesh *p)             { return p->vertex_buffer != nullptr; }
bool is_ready(ConstantBuffer *p)   { return p->buffer != nullptr; }
bool is_ready(StructuredBuffer *p) { return p->buffer != nullptr; }
bool is_ready(TextureSampler *p)   { return p->sampler != nullptr; }
bool is_ready(VertexShader *p)     { return p->valid; }
bool is_ready(PixelShader *p)      { return p->valid; }
bool is_ready(ComputeShader *p)    { return p->valid; }

void release(RenderTarget *p)     { *p = {}; }
void release(DepthBuffer *p)      { *p = {}; }
void release(Texture2D *p)        { *p = {}; }
void release(Texture3D *p)        { *p = {}; }
void release(Mesh *p)             { *p = {}; }
void release(ConstantBuffer *p)   { *p = {}; }
void release(StructuredBuffer *p) { *p = {}; }
void release(TextureSampler *p)   { *p = {}; }
void release(VertexShader *p)     { *p = {}; }
void release(PixelShader *p)      { *p = {}; }
void release(ComputeShader *p)    { *p = {}; }

}
```

- [ ] **Step 4: Build green, commit**

Run: `cmake -B build && nice -n 19 cmake --build build -j 8 && ctest --test-dir build`
Expected: green (clear kernels are lazily compiled — nothing exercises them yet; Task 5's tests do).

```bash
git add cpplib/graphics.cpp
git commit -m "graphics: buffers, textures, samplers, builtin clear kernels"
```

---

### Task 5: Compute pipelines, slot binding, dispatch, synchronous readback + GPU tests

**Files:**
- Modify: `cpplib/graphics.cpp` (append implementations)
- Create: `shaders/tests/test_write_ids.wgsl`, `shaders/tests/test_tex3d_roundtrip.wgsl`
- Test: `tests/graphics_tests.cpp`
- Modify: `CMakeLists.txt` (add `graphics_tests` target)

**Interfaces:**
- Consumes: Tasks 3-4 internals (`g_compute_slots`, `g_uniform_buffer`, `ensure_encoder`, `flush_commands`, `wait_for`).
- Produces: `get_compute_shader_from_code` (with override constants), `set_compute_shader`, `set_texture_compute`/`set_texture_sampled_compute`/`set_texture_sampler_compute`/`set_structured_buffer` + unsets, `run_compute`, `capture_structured_buffer`. This is the complete compute contract M2b's shaders run on.

- [ ] **Step 1: Implement compute pipeline creation with error scope**

```cpp
namespace graphics {

ComputeShader get_compute_shader_from_code(char *code, uint32_t code_length,
                                           const ShaderConstant *constants,
                                           uint32_t constant_count) {
    ComputeShader cs = {};
    // file_system::read_file guarantees a trailing NUL (Task 1), so `code` is
    // a valid C string; code_length is unused but kept for signature parity.
    (void)code_length;

    g_ctx.device.PushErrorScope(wgpu::ErrorFilter::Validation);

    wgpu::ShaderSourceWGSL src = {};
    src.code = code;
    wgpu::ShaderModuleDescriptor mod_desc = {};
    mod_desc.nextInChain = &src;
    wgpu::ShaderModule module = g_ctx.device.CreateShaderModule(&mod_desc);

    std::vector<wgpu::ConstantEntry> entries(constant_count);
    for (uint32_t i = 0; i < constant_count; i++) {
        entries[i] = {};
        entries[i].key = constants[i].name;
        entries[i].value = constants[i].value;
    }
    wgpu::ComputePipelineDescriptor desc = {};
    desc.compute.module = module;
    desc.compute.constantCount = constant_count;
    desc.compute.constants = entries.data();
    cs.pipeline = g_ctx.device.CreateComputePipeline(&desc);

    bool done = false, had_error = false;
    g_ctx.device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [&done, &had_error](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                            wgpu::StringView message) {
            if (type != wgpu::ErrorType::NoError) {
                had_error = true;
                fprintf(stderr, "[graphics] shader/pipeline error: %.*s\n",
                        (int)message.length, message.data);
            }
            done = true;
        });
    wait_for(&done);
    cs.valid = !had_error;
    return cs;
}

void set_compute_shader(ComputeShader *shader) { g_compute_shader = shader; }

}
```

(Callback-signature adaptation to the pinned Dawn header allowed, as before. If `PopErrorScope` with `AllowProcessEvents` isn't available in this Dawn, use the synchronous variant or `WaitAny` — the contract is: `valid == false` and a stderr message containing the Tint diagnostic when the WGSL is bad.)

- [ ] **Step 2: Implement slot binding + run_compute**

```cpp
namespace graphics {

void set_texture_compute(Texture2D *t, uint32_t slot)  { g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_TEX; g_compute_slots[slot].view = t->ua_view; }
void set_texture_compute(Texture3D *t, uint32_t slot)  { g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_TEX; g_compute_slots[slot].view = t->ua_view; }
void set_texture_sampled_compute(Texture2D *t, uint32_t slot) { g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLED_TEX; g_compute_slots[slot].view = t->sr_view; }
void set_texture_sampled_compute(Texture3D *t, uint32_t slot) { g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLED_TEX; g_compute_slots[slot].view = t->sr_view; }
void set_texture_sampler_compute(TextureSampler *s, uint32_t slot) { g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::SAMPLER; g_compute_slots[slot].sampler = s->sampler; }
void set_structured_buffer(StructuredBuffer *b, uint32_t slot) { g_compute_slots[slot] = {}; g_compute_slots[slot].kind = BoundSlot::Kind::STORAGE_BUFFER; g_compute_slots[slot].buffer = b->buffer; g_compute_slots[slot].buffer_size = b->size; }
void unset_texture_compute(uint32_t slot)          { g_compute_slots[slot] = {}; }
void unset_texture_sampled_compute(uint32_t slot)  { g_compute_slots[slot] = {}; }

void run_compute(int gx, int gy, int gz) {
    assert(g_compute_shader && g_compute_shader->valid);
    ensure_encoder();

    // Group 0: uniform at binding 0, if the shader declares one.
    // Group 1: every currently-bound slot. CONTRACT: the bound slot set must
    // exactly match the shader's @group(1) declarations — extra or missing
    // entries are a Dawn validation error (which names the binding; that is
    // the intended failure mode, better than D3D11's silent null reads).
    wgpu::BindGroup group0;
    if (g_uniform_buffer) {
        wgpu::BindGroupEntry e = {};
        e.binding = 0; e.buffer = g_uniform_buffer; e.size = g_uniform_size;
        wgpu::BindGroupDescriptor d = {};
        d.layout = g_compute_shader->pipeline.GetBindGroupLayout(0);
        d.entryCount = 1; d.entries = &e;
        group0 = g_ctx.device.CreateBindGroup(&d);
    }

    std::vector<wgpu::BindGroupEntry> entries;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) {
        const BoundSlot &s = g_compute_slots[i];
        if (s.kind == BoundSlot::Kind::NONE) continue;
        wgpu::BindGroupEntry e = {};
        e.binding = i;
        switch (s.kind) {
            case BoundSlot::Kind::STORAGE_BUFFER: e.buffer = s.buffer; e.size = s.buffer_size; break;
            case BoundSlot::Kind::STORAGE_TEX:
            case BoundSlot::Kind::SAMPLED_TEX:    e.textureView = s.view; break;
            case BoundSlot::Kind::SAMPLER:        e.sampler = s.sampler; break;
            default: break;
        }
        entries.push_back(e);
    }
    wgpu::BindGroupDescriptor d1 = {};
    d1.layout = g_compute_shader->pipeline.GetBindGroupLayout(1);
    d1.entryCount = entries.size(); d1.entries = entries.data();
    wgpu::BindGroup group1 = g_ctx.device.CreateBindGroup(&d1);

    wgpu::ComputePassEncoder pass = g_encoder.BeginComputePass();
    pass.SetPipeline(g_compute_shader->pipeline);
    if (group0) pass.SetBindGroup(0, group0);
    pass.SetBindGroup(1, group1);
    pass.DispatchWorkgroups((uint32_t)gx, (uint32_t)gy, (uint32_t)gz);
    pass.End();
}

void capture_structured_buffer(StructuredBuffer *buffer, void *mapped_data,
                               uint32_t num_elements, size_t element_size) {
    uint64_t byte_count = (uint64_t)num_elements * element_size;
    if (!buffer->readback) {
        wgpu::BufferDescriptor desc = {};
        desc.size = buffer->size;
        desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        buffer->readback = g_ctx.device.CreateBuffer(&desc);
    }
    // Flush pending compute, copy, submit, block on map — reproducing the
    // D3D11 Map(D3D11_MAP_READ) same-frame stall (inventory §6). Latency is a
    // conscious non-goal here: correctness first, matching original behavior.
    ensure_encoder();
    g_encoder.CopyBufferToBuffer(buffer->buffer, 0, buffer->readback, 0, buffer->size);
    flush_commands();

    bool done = false;
    buffer->readback.MapAsync(
        wgpu::MapMode::Read, 0, buffer->size, wgpu::CallbackMode::AllowProcessEvents,
        [&done](wgpu::MapAsyncStatus status, wgpu::StringView message) {
            if (status != wgpu::MapAsyncStatus::Success)
                fprintf(stderr, "[graphics] readback map failed: %.*s\n",
                        (int)message.length, message.data);
            done = true;
        });
    wait_for(&done);
    const void *src = buffer->readback.GetConstMappedRange(0, buffer->size);
    if (src) memcpy(mapped_data, src, byte_count);
    buffer->readback.Unmap();
}

}
```

- [ ] **Step 3: Write the test kernels**

`shaders/tests/test_write_ids.wgsl` — exercises the group-0-uniform + group-1-slot convention and an override constant:

```wgsl
override MULTIPLIER: u32 = 1u;

struct Cfg { mul : u32, add_ : u32, pad0 : u32, pad1 : u32 }
@group(0) @binding(0) var<uniform> cfg : Cfg;
@group(1) @binding(2) var<storage, read_write> out_buf : array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= arrayLength(&out_buf)) { return; }
    out_buf[gid.x] = gid.x * cfg.mul * MULTIPLIER + cfg.add_;
}
```

`shaders/tests/test_tex3d_roundtrip.wgsl` — two entry points would need two pipelines; keep it one kernel that reads the 3D storage texture into a buffer (the write side is `clear_texture`):

```wgsl
@group(1) @binding(0) var tex : texture_storage_3d<r32float, read>;
@group(1) @binding(1) var<storage, read_write> out_buf : array<f32>;

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(tex);
    if (gid.x >= dims.x || gid.y >= dims.y || gid.z >= dims.z) { return; }
    let idx = gid.x + gid.y * dims.x + gid.z * dims.x * dims.y;
    out_buf[idx] = textureLoad(tex, gid).x;
}
```

- [ ] **Step 4: Write the GPU test**

```cpp
// tests/graphics_tests.cpp
#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
#include <cstdio>
#include <cstring>

// TEST_SHADER_DIR is injected by CMake as an absolute path to shaders/tests.
static char *load_shader(const char *name, file_system::File *out) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", TEST_SHADER_DIR, name);
    *out = file_system::read_file(path);
    assert(out->data && "test shader file missing");
    return (char *)out->data;
}

int main() {
    bool ok = graphics::init();   // headless: no init_swap_chain
    assert(ok);

    // --- Test 1: uniform + storage buffer + override constant + readback ---
    {
        file_system::File f;
        char *code = load_shader("test_write_ids.wgsl", &f);
        graphics::ShaderConstant consts[] = {{"MULTIPLIER", 3.0}};
        graphics::ComputeShader cs =
            graphics::get_compute_shader_from_code(code, (uint32_t)f.size, consts, 1);
        assert(graphics::is_ready(&cs));
        file_system::release_file(f);

        const uint32_t N = 1000;
        graphics::StructuredBuffer buf = graphics::get_structured_buffer(sizeof(uint32_t), N);
        graphics::ConstantBuffer cb = graphics::get_constant_buffer(16);
        uint32_t cfg[4] = {5, 7, 0, 0};   // mul=5, add=7
        graphics::update_constant_buffer(&cb, cfg);
        graphics::set_constant_buffer(&cb, 0);
        graphics::set_structured_buffer(&buf, 2);
        graphics::set_compute_shader(&cs);
        graphics::run_compute((N + 63) / 64, 1, 1);
        graphics::unset_texture_compute(2);

        uint32_t result[N];
        graphics::capture_structured_buffer(&buf, result, N, sizeof(uint32_t));
        for (uint32_t i = 0; i < N; i++)
            assert(result[i] == i * 5u * 3u + 7u);
        printf("graphics_tests: write_ids (uniform+override+readback) passed\n");
        graphics::release(&buf); graphics::release(&cb); graphics::release(&cs);
    }

    // --- Test 2: 3D storage texture clear + read into buffer ---
    {
        file_system::File f;
        char *code = load_shader("test_tex3d_roundtrip.wgsl", &f);
        graphics::ComputeShader cs =
            graphics::get_compute_shader_from_code(code, (uint32_t)f.size);
        assert(graphics::is_ready(&cs));
        file_system::release_file(f);

        const uint32_t W = 8, H = 8, D = 8;
        graphics::Texture3D tex =
            graphics::get_texture3D(nullptr, W, H, D, graphics::Format::R32_FLOAT);
        assert(graphics::is_ready(&tex));
        graphics::clear_texture(&tex, 2.5f);

        graphics::StructuredBuffer buf =
            graphics::get_structured_buffer(sizeof(float), W * H * D);
        graphics::set_texture_compute(&tex, 0);   // NOTE: binds ua_view; the
        // kernel declares `read` access — if Dawn rejects a read_write-created
        // view bound as read (it should not; access is declared shader-side),
        // record the finding, it affects M2b's decay shader (D1 narrowing).
        graphics::set_structured_buffer(&buf, 1);
        graphics::set_compute_shader(&cs);
        graphics::run_compute(W / 4, H / 4, D / 4);
        graphics::unset_texture_compute(0);
        graphics::unset_texture_compute(1);

        float result[W * H * D];
        graphics::capture_structured_buffer(&buf, result, W * H * D, sizeof(float));
        for (uint32_t i = 0; i < W * H * D; i++)
            assert(result[i] == 2.5f);
        printf("graphics_tests: tex3d clear+roundtrip passed\n");
        graphics::release(&buf); graphics::release(&tex); graphics::release(&cs);
    }

    // --- Test 3: invalid WGSL yields is_ready == false, no crash ---
    {
        char bad[] = "this is not wgsl";
        graphics::ComputeShader cs = graphics::get_compute_shader_from_code(bad, sizeof(bad));
        assert(!graphics::is_ready(&cs));
        printf("graphics_tests: invalid shader rejected cleanly\n");
    }

    graphics::release();
    printf("graphics_tests: all passed\n");
    return 0;
}
```

CMakeLists.txt additions:

```cmake
add_executable(graphics_tests
  tests/graphics_tests.cpp
  cpplib/graphics.cpp
  cpplib/file_system.cpp
  cpplib/gpu/gpu_context.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
)
target_compile_features(graphics_tests PRIVATE cxx_std_17)
target_compile_definitions(graphics_tests PRIVATE
  TEST_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/tests")
target_include_directories(graphics_tests PRIVATE ${CMAKE_SOURCE_DIR}/cpplib)
target_link_libraries(graphics_tests PRIVATE webgpu_dawn webgpu_glfw glfw)
add_test(NAME graphics_tests COMMAND graphics_tests)
```

(platform.cpp/input.cpp are linked only because gpu_context.cpp references `Window`; if it links without them, drop them.)

- [ ] **Step 5: Build, run tests, commit**

Run: `cmake -B build && nice -n 19 cmake --build build -j 8 && ctest --test-dir build`
Expected: `file_system_tests`, `cpplib_tests`, `graphics_tests` all PASS. Test 2's storage-view-access note: if it fails validation, fix by creating the bind entry from a view whose usage matches (and record it — M2b needs to know).

```bash
git add cpplib/graphics.cpp shaders/tests tests/graphics_tests.cpp CMakeLists.txt
git commit -m "graphics: compute pipelines, slot-based bind groups, dispatch, sync readback + GPU tests"
```

---

### Task 6: Render-side stubs + full-surface link check

**Files:**
- Modify: `cpplib/graphics.cpp` (append the remaining declarations as inert implementations)

**Interfaces:**
- Consumes: Task 3's header declarations and internal state.
- Produces: every remaining `graphics.h` declaration defined — the full library links. M3 replaces the draw path; M5 replaces the save/capture path.

- [ ] **Step 1: Implement the stubs**

```cpp
namespace graphics {

static void warn_once(const char *what) {
    // one stderr line per distinct stub, first call only (`what` is always a
    // string literal, so pointer identity is a valid key)
    static const char *seen[8] = {};
    for (int i = 0; i < 8; i++) {
        if (seen[i] == what) return;
        if (!seen[i]) {
            seen[i] = what;
            fprintf(stderr, "[graphics] %s: stub until M3/M5\n", what);
            return;
        }
    }
}

Mesh get_mesh(void *vertices, uint32_t vertex_count, uint32_t vertex_stride,
              void *indices, uint32_t index_count, uint32_t index_byte_size,
              Topology topology) {
    // Real vertex buffer now (M3 draws it); index path unused by main.cpp
    // (inventory §1: both meshes are non-indexed) — assert it stays that way.
    assert(indices == nullptr && index_count == 0 && "fork: non-indexed meshes only");
    (void)index_byte_size;
    Mesh m = {};
    wgpu::BufferDescriptor desc = {};
    desc.size = ((uint64_t)vertex_count * vertex_stride + 3u) & ~3ull;
    desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
    m.vertex_buffer = g_ctx.device.CreateBuffer(&desc);
    g_ctx.queue.WriteBuffer(m.vertex_buffer, 0, vertices,
                            (uint64_t)vertex_count * vertex_stride);
    m.vertex_stride = vertex_stride;
    m.vertex_count = vertex_count;
    m.topology = topology;
    return m;
}

void draw_mesh(Mesh *mesh) { (void)mesh; warn_once("draw_mesh"); }

VertexShader get_vertex_shader_from_code(char *code, uint32_t code_length) {
    (void)code; (void)code_length;
    VertexShader vs = {}; vs.valid = true;   // M3 compiles real WGSL here
    return vs;
}
PixelShader get_pixel_shader_from_code(char *code, uint32_t code_length) {
    (void)code; (void)code_length;
    PixelShader ps = {}; ps.valid = true;
    return ps;
}
void set_vertex_shader(VertexShader *shader) { (void)shader; }
void set_pixel_shader(PixelShader *shader)   { (void)shader; }

void set_texture(Texture2D *t, uint32_t slot)  { (void)t; (void)slot; }
void set_texture(Texture3D *t, uint32_t slot)  { (void)t; (void)slot; }
void unset_texture(uint32_t slot)              { (void)slot; }
void set_texture_sampler(TextureSampler *s, uint32_t slot) { (void)s; (void)slot; }

void set_blend_state(BlendType type) { g_blend = type; }   // consumed by M3 draws

Texture2D load_texture2D(std::string filename) {
    // Palette TGAs load for real in M4 via stb_image; until then a 1x1 white
    // texture keeps bind groups valid.
    warn_once("load_texture2D");
    (void)filename;
    float white[4] = {1.f, 1.f, 1.f, 1.f};
    return get_texture2D(white, 1, 1, Format::RGBA32_FLOAT);
}

void save_texture3D(Texture3D *texture, std::string filename) {
    (void)texture; (void)filename; warn_once("save_texture3D");
}
void save_texture2D_HDR(Texture2D *texture, std::string filename) {
    (void)texture; (void)filename; warn_once("save_texture2D_HDR");
}
uint32_t capture_current_frame() { warn_once("capture_current_frame"); return 0; }

}
```

- [ ] **Step 2: Whole-library link check**

Every `graphics.h` declaration must now have a definition. Verify by compiling a translation unit that references the API surface — the `graphics_tests` target already links the library; additionally run:

```bash
nm build/CMakeFiles/graphics_tests.dir/cpplib/graphics.cpp.o | grep " U .*graphics" || echo "no undefined graphics symbols"
```

Run: `cmake -B build && nice -n 19 cmake --build build -j 8 && ctest --test-dir build && (cd build && ./polyphorm & sleep 3; kill %1)`
Expected: all tests green; M1 window still runs.

- [ ] **Step 3: Commit**

```bash
git add cpplib/graphics.cpp
git commit -m "graphics: render-side stubs (draw/shaders/save inert until M3/M5), mesh buffers real"
```
