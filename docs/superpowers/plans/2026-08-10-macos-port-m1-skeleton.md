# macOS Port — M1 Skeleton (fork cleanup, CMake+Dawn+GLFW, window/device/clear) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A CMake-built macOS binary that opens a GLFW window, brings up a Dawn WebGPU device (with `float32-filterable` verified), clears the surface each frame, pumps the ported `platform::`/`input::` event system, and exits on Esc.

**Architecture:** Plan 1 of the spec's M1–M5 chain (spec: `docs/superpowers/specs/2026-08-10-macos-webgpu-port-design.md`). Dead Windows subsystems are deleted; `platform.*`/`input.*` are ported to GLFW preserving their exact public APIs so `main.cpp` compiles unmodified in M2; a new internal `cpplib/gpu/GpuContext` owns instance/adapter/device/surface (M2 wraps it behind the legacy `graphics::` API). The original `main.cpp` stays untouched and unbuilt this milestone; a temporary `main_m1.cpp` is the entry point.

**Tech Stack:** C++17, CMake ≥ 3.24, Dawn (pinned, `DAWN_FETCH_DEPENDENCIES=ON`, monolithic `webgpu_dawn` + `webgpu_glfw` + its vendored GLFW), CTest for CPU-side units.

## Global Constraints

- Complete fork: deletions are permanent, no upstream-compat shims.
- `platform.h` / `input.h` public signatures must not change (M2 compiles `main.cpp` against them verbatim) — except `Ticks`, which becomes `uint64_t` (nanoseconds; `LARGE_INTEGER` is Windows-only and `main.cpp` never touches its fields directly).
- Device init MUST fatal-error with the feature/limit name if `float32-filterable` is unavailable (spec: Error handling).
- Every commit leaves `cmake --build build` green from a clean configure.
- Commit messages end with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

### Task 1: Fork cleanup — delete dead subsystems

**Files:**
- Delete: `cpplib/ovr.cpp`, `cpplib/ovr.h`, `cpplib/oculus/` (dir), `cpplib/audio.cpp`, `cpplib/audio.h`, `cpplib/stb_vorbis.c`, `cpplib/fbx_loader.cpp`, `cpplib/fbx_loader.h`, `cpplib/resources.cpp`, `cpplib/resources.h`, `cpplib/freetype/` (dir), `builder/` (dir), `build_and_run.bat`, `polyphorm.build`, `bin/freetype271MT.dll`, `bin/cs_agents_sort.hlsl`, `bin/ps_volume_halocolor.hlsl` (stale copies; canonical files live in `shaders/`)
- Keep (deleted later, in M2's ImGui swap, because `main.cpp` still `#include`s them): `cpplib/ui.*`, `cpplib/font.*`, `cpplib/fonts/`

**Interfaces:**
- Consumes: nothing.
- Produces: a tree where every remaining `.cpp` under `cpplib/` is either kept-as-is (`maths`, `memory`, `random`, `logging`, `parsers`, `file_system`, `input`, `ui`, `font`) or scheduled for port (`platform`, `graphics`).

- [ ] **Step 1: Verify nothing kept references the doomed files**

Run: `grep -rn 'ovr\.h\|audio\.h\|fbx_loader\.h\|resources\.h\|stb_vorbis' main.cpp cpplib/*.cpp cpplib/*.h --include='*' | grep -v '^cpplib/ovr\|^cpplib/audio\|^cpplib/fbx_loader\|^cpplib/resources'`
Expected: no output. (If `main.cpp` includes any, stop and re-check the spec — it should not.)

- [ ] **Step 2: Delete**

```bash
git rm -r cpplib/ovr.cpp cpplib/ovr.h cpplib/oculus cpplib/audio.cpp cpplib/audio.h \
  cpplib/stb_vorbis.c cpplib/fbx_loader.cpp cpplib/fbx_loader.h \
  cpplib/resources.cpp cpplib/resources.h cpplib/freetype builder \
  build_and_run.bat polyphorm.build bin/freetype271MT.dll \
  bin/cs_agents_sort.hlsl bin/ps_volume_halocolor.hlsl
```

- [ ] **Step 3: Commit**

```bash
git commit -m "chore: delete Windows-only and dead subsystems (VR, audio, FBX, freetype, custom builder)

Complete-fork cleanup per the port spec. ui/font survive until the M2
ImGui swap because main.cpp still includes them.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: CMake skeleton with pinned Dawn

**Files:**
- Create: `CMakeLists.txt`, `main_m1.cpp` (temporary entry; deleted when `main.cpp` comes online in M2/M3), `.gitignore` addition for `build/`

**Interfaces:**
- Consumes: nothing.
- Produces: targets `polyphorm` (app) and, from Task 3 on, `cpplib_tests` (CTest). Dawn targets `webgpu_dawn`, `webgpu_glfw`, `glfw` available to link.

- [ ] **Step 1: Write CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.24)
project(polyphorm CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Dawn, pinned. DAWN_FETCH_DEPENDENCIES makes Dawn pull its own deps
# (incl. GLFW) without depot_tools. First configure+build is heavy
# (~10-20 min); afterwards it is cached.
set(DAWN_FETCH_DEPENDENCIES ON)
set(DAWN_USE_GLFW ON)
set(DAWN_BUILD_SAMPLES OFF)
set(TINT_BUILD_TESTS OFF)
set(TINT_BUILD_CMD_TOOLS OFF)
FetchContent_Declare(
  dawn
  GIT_REPOSITORY https://github.com/google/dawn
  GIT_TAG        chromium/6800
  GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(dawn)

add_executable(polyphorm
  main_m1.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/maths.cpp
  cpplib/memory.cpp
  cpplib/random.cpp
  cpplib/logging.cpp
  cpplib/file_system.cpp
  cpplib/gpu/gpu_context.cpp
)
target_include_directories(polyphorm PRIVATE cpplib)
target_link_libraries(polyphorm PRIVATE webgpu_dawn webgpu_glfw glfw)
```

Note: `cpplib/gpu/gpu_context.cpp` doesn't exist until Task 4 and `platform.cpp` is still Win32 until Task 3 — the build is NOT expected to succeed until Task 4. This task only proves the Dawn fetch + configure works, so comment those four `cpplib/` lines plus `gpu_context.cpp` out for now with `# uncommented in Task 3/4`.

- [ ] **Step 2: Write the temporary entry `main_m1.cpp`**

```cpp
// Temporary M1 entry point. Replaced by the real main.cpp in M2/M3.
int main() { return 0; }
```

- [ ] **Step 3: Configure and build**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
Expected: configure fetches Dawn and succeeds; build produces `build/polyphorm`. If the `chromium/6800` tag does not exist at fetch time, run `git ls-remote --heads https://github.com/google/dawn 'chromium/*' | tail -5`, pick the newest branch, update `GIT_TAG`, and record the change in the commit message — the pin must be a value that fetched successfully, never a moving branch left implicit.

- [ ] **Step 4: Ignore the build dir and commit**

```bash
echo 'build/' >> .gitignore
git add CMakeLists.txt main_m1.cpp .gitignore
git commit -m "build: CMake skeleton with pinned Dawn (fetched deps, GLFW, monolithic webgpu_dawn)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Port platform.* and input.* to GLFW

**Files:**
- Modify: `cpplib/platform.h` (de-Windows the types, same API), `cpplib/platform.cpp` (full rewrite over GLFW), `cpplib/input.cpp` (should need zero changes — verify)
- Create: `tests/platform_input_tests.cpp`
- Modify: `CMakeLists.txt` (uncomment platform/input/maths/etc. sources; add `cpplib_tests` target + CTest)

**Interfaces:**
- Consumes: GLFW (via Dawn's vendored copy).
- Produces (unchanged public API from `platform.h`/`input.h`, new type mapping):
  - `struct Window { GLFWwindow *window_handle; uint32_t window_width; uint32_t window_height; }` (forward-declare `GLFWwindow`; macro becomes `#define IS_WINDOW_VALID(window) ((window).window_handle != nullptr)`)
  - `typedef uint64_t Ticks;` — nanoseconds since an arbitrary epoch; `platform::get_tick_frequency()` returns `1000000000ull`
  - `platform::get_window / set_window_title / is_window_valid / get_event / show_cursor / hide_cursor / get_ticks / get_tick_frequency / get_dt_from_tick_difference`, `timer::get / start / end / checkpoint` — signatures exactly as in the current headers
  - Task 4 additionally relies on: `Window.window_handle` being the `GLFWwindow*` to create the WebGPU surface from.

- [ ] **Step 1: Write the failing unit tests**

`tests/platform_input_tests.cpp` — plain `assert` + exit-code style (no framework; matches the repo's zero-dependency habits):

```cpp
#include "platform.h"
#include "input.h"
#include <cassert>
#include <cstring>

static Event make_key_event(EventType type, KeyCode code) {
    Event e; e.type = type;
    KeyPressedData d{code};
    std::memcpy(e.data, &d, sizeof(d));
    return e;
}

int main() {
    // Tick math: 2.5e9 ns at 1e9 Hz is 2.5 s.
    assert(platform::get_tick_frequency() == 1000000000ull);
    float dt = platform::get_dt_from_tick_difference(1000000000ull, 3500000000ull,
                                                     platform::get_tick_frequency());
    assert(dt > 2.499f && dt < 2.501f);

    // get_ticks is monotonic non-decreasing.
    Ticks a = platform::get_ticks();
    Ticks b = platform::get_ticks();
    assert(b >= a);

    // Input state machine: key F2 pressed exactly once per down transition.
    input::reset();
    Event down = make_key_event(KEY_DOWN, F2);
    input::register_event(&down);
    assert(input::key_pressed(F2));
    assert(!input::key_pressed(F3));
    input::reset();
    assert(!input::key_pressed(F2));  // pressed is edge-triggered, cleared by reset

    // Mouse: delta accumulates from move events and zeroes on reset.
    Event mv; mv.type = MOUSE_MOVE;
    MouseMoveData md{120.0f, 80.0f};
    std::memcpy(mv.data, &md, sizeof(md));
    input::register_event(&mv);
    assert(input::mouse_position().x == 120.0f);
    Event lb; lb.type = MOUSE_LBUTTON_DOWN; std::memcpy(lb.data, &md, sizeof(md));
    input::register_event(&lb);
    assert(input::mouse_left_button_pressed() && input::mouse_left_button_down());
    input::reset();
    assert(!input::mouse_left_button_pressed());
    assert(input::mouse_left_button_down());  // held state survives reset

    return 0;
}
```

Add to `CMakeLists.txt`:

```cmake
enable_testing()
add_executable(cpplib_tests
  tests/platform_input_tests.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/maths.cpp
)
target_include_directories(cpplib_tests PRIVATE cpplib)
target_link_libraries(cpplib_tests PRIVATE glfw)
add_test(NAME cpplib_tests COMMAND cpplib_tests)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target cpplib_tests`
Expected: FAIL — `platform.cpp` still includes `<windowsx.h>` and does not compile on macOS.

- [ ] **Step 3: Port `platform.h`**

Replace the two Windows lines only:

```cpp
// was: #include <Windows.h>   +   typedef LARGE_INTEGER Ticks;
struct GLFWwindow;
typedef uint64_t Ticks;  // nanoseconds; frequency fixed at 1e9
```

and change `HWND window_handle;` → `GLFWwindow *window_handle;`, plus the macro:
`#define IS_WINDOW_VALID(window) ((window).window_handle != nullptr)`. Everything else in the header is untouched.

- [ ] **Step 4: Rewrite `platform.cpp` over GLFW**

```cpp
#include "platform.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstring>
#include <deque>

// GLFW callbacks feed this queue; platform::get_event drains it once per
// frame, preserving the original Win32-message-pump contract main.cpp
// is written against.
static std::deque<Event> g_events;
static GLFWwindow *g_glfw_window = nullptr;  // the single live window; set in get_window
static bool g_exit_pushed = false;           // EXIT is delivered exactly once

template <typename T>
static void push_event(EventType type, const T &payload) {
    Event e; e.type = type;
    static_assert(sizeof(T) <= sizeof(e.data), "Event payload too large");
    std::memcpy(e.data, &payload, sizeof(T));
    g_events.push_back(e);
}
static void push_event(EventType type) { Event e; e.type = type; g_events.push_back(e); }

static KeyCode map_key(int key) {
    switch (key) {
        case GLFW_KEY_ESCAPE: return ESC;
        case GLFW_KEY_F1: return F1;   case GLFW_KEY_F2: return F2;
        case GLFW_KEY_F3: return F3;   case GLFW_KEY_F4: return F4;
        case GLFW_KEY_F5: return F5;   case GLFW_KEY_F6: return F6;
        case GLFW_KEY_F7: return F7;   case GLFW_KEY_F8: return F8;
        case GLFW_KEY_F9: return F9;   case GLFW_KEY_F10: return F10;
        case GLFW_KEY_1: return NUM1;  case GLFW_KEY_2: return NUM2;
        case GLFW_KEY_3: return NUM3;  case GLFW_KEY_4: return NUM4;
        case GLFW_KEY_5: return NUM5;  case GLFW_KEY_6: return NUM6;
        default: return OTHER;
    }
}

namespace platform {

Window get_window(char *window_name, uint32_t window_width, uint32_t window_height) {
    Window window = {};
    if (!glfwInit()) return window;
    // No GL context — the surface belongs to WebGPU (Task 4).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);  // original window is fixed-size
    GLFWwindow *handle = glfwCreateWindow((int)window_width, (int)window_height,
                                          window_name, nullptr, nullptr);
    if (!handle) { glfwTerminate(); return window; }
    window.window_handle = handle;
    window.window_width = window_width;
    window.window_height = window_height;

    glfwSetKeyCallback(handle, [](GLFWwindow *, int key, int, int action, int) {
        if (action == GLFW_PRESS)  push_event(KEY_DOWN, KeyPressedData{map_key(key)});
        if (action == GLFW_RELEASE) push_event(KEY_UP,  KeyPressedData{map_key(key)});
    });
    glfwSetCursorPosCallback(handle, [](GLFWwindow *, double x, double y) {
        push_event(MOUSE_MOVE, MouseMoveData{(float)x, (float)y});
    });
    glfwSetMouseButtonCallback(handle, [](GLFWwindow *w, int button, int action, int) {
        double x, y; glfwGetCursorPos(w, &x, &y);
        MouseMoveData at{(float)x, (float)y};
        if (button == GLFW_MOUSE_BUTTON_LEFT)
            push_event(action == GLFW_PRESS ? MOUSE_LBUTTON_DOWN : MOUSE_LBUTTON_UP, at);
        if (button == GLFW_MOUSE_BUTTON_RIGHT)
            push_event(action == GLFW_PRESS ? MOUSE_RBUTTON_DOWN : MOUSE_RBUTTON_UP, at);
    });
    glfwSetScrollCallback(handle, [](GLFWwindow *, double, double yoff) {
        push_event(MOUSE_WHEEL, MouseWheelData{(float)yoff});
    });

    g_glfw_window = handle;
    return window;
}

bool set_window_title(Window &window, const char *window_title) {
    if (!window.window_handle) return false;
    glfwSetWindowTitle(window.window_handle, window_title);
    return true;
}

bool is_window_valid(Window *window) {
    return window && window->window_handle != nullptr;
}

bool get_event(Event *event) {
    // First call each frame pumps the OS queue; subsequent calls drain ours —
    // preserving the original drain-per-frame Win32-message-pump contract.
    if (g_events.empty()) {
        glfwPollEvents();
        if (g_glfw_window && glfwWindowShouldClose(g_glfw_window) && !g_exit_pushed) {
            push_event(EXIT);
            g_exit_pushed = true;
        }
    }
    if (g_events.empty()) return false;
    *event = g_events.front();
    g_events.pop_front();
    return true;
}

void show_cursor() {
    if (g_glfw_window) glfwSetInputMode(g_glfw_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}
void hide_cursor() {
    if (g_glfw_window) glfwSetInputMode(g_glfw_window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
}

Ticks get_ticks() {
    return (Ticks)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
Ticks get_tick_frequency() { return 1000000000ull; }

float get_dt_from_tick_difference(Ticks t1, Ticks t2, Ticks frequency) {
    return (float)((double)(t2 - t1) / (double)frequency);
}

}  // namespace platform

namespace timer {
Timer get() { Timer t; t.frequency = platform::get_tick_frequency(); t.start = 0; return t; }
void start(Timer *t) { t->start = platform::get_ticks(); }
float end(Timer *t) {
    return platform::get_dt_from_tick_difference(t->start, platform::get_ticks(), t->frequency);
}
float checkpoint(Timer *t) {
    Ticks now = platform::get_ticks();
    float dt = platform::get_dt_from_tick_difference(t->start, now, t->frequency);
    t->start = now;
    return dt;
}
}  // namespace timer
```

Implementation notes (not optional):
- Verify against the original `platform.cpp` how `get_dt_from_tick_difference` orders `t1`/`t2` (the original divides `(t2 - t1)` by frequency — confirm by reading the Win32 version before deleting it) and keep the same order; the unit test in Step 1 encodes `(t2 - t1)`.
- `input.cpp`: build it unmodified. It includes `platform.h` only for `Event`/`KeyCode`, which still exist.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build -j --target cpplib_tests && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add cpplib/platform.h cpplib/platform.cpp tests/platform_input_tests.cpp CMakeLists.txt
git commit -m "port: platform layer Win32 -> GLFW, same public API; input.cpp unchanged

Ticks become steady_clock nanoseconds (freq 1e9). Event pump preserves
the drain-per-frame contract main.cpp is written against.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: GpuContext — Dawn instance/adapter/device/surface bring-up

**Files:**
- Create: `cpplib/gpu/gpu_context.h`, `cpplib/gpu/gpu_context.cpp`
- Modify: `CMakeLists.txt` (uncomment `gpu_context.cpp`)

**Interfaces:**
- Consumes: `Window` from Task 3 (`window.window_handle` is the `GLFWwindow*`).
- Produces (M2's `graphics::` shim wraps exactly this):

```cpp
// cpplib/gpu/gpu_context.h
#pragma once
#include <webgpu/webgpu_cpp.h>
struct Window;

struct GpuContext {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Surface surface;
    wgpu::TextureFormat surface_format = wgpu::TextureFormat::BGRA8Unorm;
    uint32_t width = 0, height = 0;
};

namespace gpu {
// Fatal (prints the missing feature/limit name and aborts) on any failure —
// spec "Error handling": startup capability misses must be loud and named.
GpuContext init(Window *window);
// Acquire the current surface texture, run a render pass that clears it
// to (r, g, b), submit, and present. M2 replaces this with the full
// graphics:: pass API; nothing else may grow onto it.
void clear_and_present(GpuContext *ctx, float r, float g, float b);
}
```

- [ ] **Step 1: Write `gpu_context.cpp`**

```cpp
#include "gpu/gpu_context.h"
#include "platform.h"
#include <webgpu/webgpu_glfw.h>
#include <cstdio>
#include <cstdlib>

namespace gpu {

static void fatal(const char *what) {
    std::fprintf(stderr, "[gpu] FATAL: %s\n", what);
    std::abort();
}

GpuContext init(Window *window) {
    GpuContext ctx;
    ctx.instance = wgpu::CreateInstance(nullptr);
    if (!ctx.instance) fatal("wgpu::CreateInstance failed");

    // Synchronous adapter/device acquisition: Dawn's async requests are
    // pumped to completion with ProcessEvents. The names of the callback
    // enums drift between Dawn revisions; the pinned revision in
    // CMakeLists.txt is the source of truth — keep the semantics
    // (request, pump until the callback fired) and fix names per the
    // compiler's suggestions if the pin ever moves.
    wgpu::RequestAdapterOptions adapter_opts = {};
    adapter_opts.powerPreference = wgpu::PowerPreference::HighPerformance;
    bool done = false;
    ctx.instance.RequestAdapter(
        &adapter_opts, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView msg) {
            if (status != wgpu::RequestAdapterStatus::Success)
                fatal("RequestAdapter failed");
            ctx.adapter = adapter;
            done = true;
        });
    while (!done) ctx.instance.ProcessEvents();

    // float32-filterable is required by the volume renderer (spec: GPU
    // resource mapping — trace is r32float and sampled with a linear
    // sampler). Fail HERE, with the feature's name, not later.
    if (!ctx.adapter.HasFeature(wgpu::FeatureName::Float32Filterable))
        fatal("adapter lacks required feature: float32-filterable");

    wgpu::FeatureName required[] = { wgpu::FeatureName::Float32Filterable };
    wgpu::DeviceDescriptor dev_desc = {};
    dev_desc.requiredFeatures = required;
    dev_desc.requiredFeatureCount = 1;
    dev_desc.SetUncapturedErrorCallback(
        [](const wgpu::Device &, wgpu::ErrorType type, wgpu::StringView msg) {
            std::fprintf(stderr, "[gpu] uncaptured error (%d): %.*s\n",
                         (int)type, (int)msg.length, msg.data);
        });
    dev_desc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device &, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            std::fprintf(stderr, "[gpu] device lost (%d): %.*s\n",
                         (int)reason, (int)msg.length, msg.data);
        });

    done = false;
    ctx.adapter.RequestDevice(
        &dev_desc, wgpu::CallbackMode::AllowProcessEvents,
        [&](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView msg) {
            if (status != wgpu::RequestDeviceStatus::Success)
                fatal("RequestDevice failed");
            ctx.device = device;
            done = true;
        });
    while (!done) ctx.instance.ProcessEvents();
    ctx.queue = ctx.device.GetQueue();

    ctx.surface = wgpu::glfw::CreateSurfaceForWindow(ctx.instance, window->window_handle);
    if (!ctx.surface) fatal("CreateSurfaceForWindow failed");
    ctx.width = window->window_width;
    ctx.height = window->window_height;

    wgpu::SurfaceConfiguration config = {};
    config.device = ctx.device;
    config.format = ctx.surface_format;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = ctx.width;
    config.height = ctx.height;
    config.presentMode = wgpu::PresentMode::Fifo;  // = the original's Present(1,0) vsync
    ctx.surface.Configure(&config);
    return ctx;
}

void clear_and_present(GpuContext *ctx, float r, float g, float b) {
    wgpu::SurfaceTexture surface_tex;
    ctx->surface.GetCurrentTexture(&surface_tex);
    if (surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surface_tex.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
        fatal("GetCurrentTexture failed");

    wgpu::RenderPassColorAttachment att = {};
    att.view = surface_tex.texture.CreateView();
    att.loadOp = wgpu::LoadOp::Clear;
    att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {r, g, b, 1.0};
    wgpu::RenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;

    wgpu::CommandEncoder enc = ctx->device.CreateCommandEncoder();
    enc.BeginRenderPass(&rp).End();
    wgpu::CommandBuffer cmd = enc.Finish();
    ctx->queue.Submit(1, &cmd);
    ctx->surface.Present();
    ctx->instance.ProcessEvents();
}

}  // namespace gpu
```

- [ ] **Step 2: Build**

Run: `cmake --build build -j`
Expected: compiles and links. Expect enum/callback-signature drift vs the pinned Dawn revision — resolve per compiler messages, preserving semantics (the comment in the code says exactly this).

- [ ] **Step 3: Commit**

```bash
git add cpplib/gpu/gpu_context.h cpplib/gpu/gpu_context.cpp CMakeLists.txt
git commit -m "gpu: Dawn instance/adapter/device/surface bring-up with float32-filterable gate

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: M1 loop — window, events, animated clear, Esc exit

**Files:**
- Modify: `main_m1.cpp`

**Interfaces:**
- Consumes: everything from Tasks 3–4.
- Produces: the demo-able M1 milestone; nothing downstream consumes `main_m1.cpp` (M2 deletes it).

- [ ] **Step 1: Write the loop**

```cpp
#include "platform.h"
#include "input.h"
#include "gpu/gpu_context.h"
#include <cmath>
#include <cstdio>

int main() {
    Window window = platform::get_window((char *)"Polyphorm [M1]", 1280, 720);
    if (!IS_WINDOW_VALID(window)) { std::fprintf(stderr, "window creation failed\n"); return 1; }
    GpuContext ctx = gpu::init(&window);

    Timer frame_timer = timer::get();
    timer::start(&frame_timer);
    float t = 0.0f, frame_ms = 0.0f;
    bool running = true;
    while (running) {
        input::reset();
        Event event;
        while (platform::get_event(&event)) {
            input::register_event(&event);
            if (event.type == EXIT) running = false;
        }
        if (input::key_pressed(ESC)) running = false;

        float dt = timer::checkpoint(&frame_timer);
        t += dt;
        frame_ms = 0.9f * frame_ms + 0.1f * dt * 1000.0f;  // the original's EMA style
        char title[128];
        std::snprintf(title, sizeof(title), "Polyphorm [M1] [%.1f ms]", frame_ms);
        platform::set_window_title(window, title);

        // Slow teal<->purple pulse proves per-frame present + timing.
        gpu::clear_and_present(&ctx, 0.1f + 0.1f * std::sin(t), 0.2f, 0.35f);
    }
    return 0;
}
```

- [ ] **Step 2: Build and run the smoke check (manual)**

Run: `cmake --build build -j && ./build/polyphorm`
Expected: a 1280×720 window titled `Polyphorm [M1] [x.x ms]` with a slowly pulsing blue-ish clear color, ~16.7 ms/frame (Fifo vsync at 60 Hz), Esc and the close button both exit cleanly, no `[gpu]` errors on stderr. **Look at the window** — a black frame or instant exit is a failure even if the exit code is 0.

- [ ] **Step 3: Run the unit tests once more, then commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS.

```bash
git add main_m1.cpp
git commit -m "feat(m1): window + device + animated clear + event/input loop, Esc exits

M1 milestone of the port spec: build system, platform layer, and Dawn
device bring-up are live. main.cpp remains unbuilt until M2.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-review notes (kept for the executor)

- Spec coverage for M1 only: cleanup ✓ (Task 1), CMake+Dawn pinned ✓ (Task 2), platform/input port with API freeze ✓ (Task 3), device with float32-filterable fatal gate + error callbacks ✓ (Task 4), runnable demo ✓ (Task 5). Shaders/graphics-API/ImGui/export/validation are M2+ plans by design.
- The Dawn pin (`chromium/6800`) and the callback enum names are the two places most likely to need adjustment at execution time; both carry explicit in-plan instructions for how to adjust and record the change.
- `ui.*`/`font.*` intentionally survive Task 1 — deleting them belongs to the M2 ImGui swap that edits their `main.cpp` call sites.
