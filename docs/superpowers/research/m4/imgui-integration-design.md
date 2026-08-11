# M4a: Dear ImGui integration design

Repo `/Users/rulkens/Development/vendor/cpp/Polyphorm`, branch `macos-webgpu-port`, HEAD
`0fef1be`. M4a's job: turn the stubbed `ui::` namespace (`cpplib/ui_stub.cpp`, currently
every function is a no-op returning a default) into a real Dear ImGui implementation,
without changing `ui.h`'s function signatures (main.cpp's ~50 `ui::` call sites must keep
compiling unmodified). Scope is UI plumbing only — the volume/path-tracing shaders stay
stubbed until M4b; this document's job is to make sure the newly-live toggles that would
reach those stubs fail *safely* rather than crash.

This is a design document. No code was changed to produce it.

---

## 0. Carry-forward check: does the M2 research still hold?

Read in full: `docs/superpowers/research/m2/imgui-integration.md` (pins, backend
inventory, install order, 8 pitfalls) and `docs/superpowers/research/m3/m3-carryovers.md`
(M4 obligations #6/#7 plus the graphics:: fix list).

- **Dawn pin unchanged.** `CMakeLists.txt:36` still reads `GIT_TAG v20260807.193620`,
  exactly what the M2 doc verified against. No re-verification of the imgui/implot
  *tags* was done here (nor is it needed): the M2 doc pins by **commit hash**
  (`01380c579715e62fb9a8d6ec0502c4ea83bfde6e` for imgui `v1.92.9`,
  `524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39` for implot `v1.0`), not "latest" — a fixed
  commit doesn't drift with the calendar. Carried forward unchanged.
- **`imgui_impl_wgpu`/`imgui_impl_glfw` compatibility verdicts, the 8 pitfalls, the
  `IMGUI_IMPL_WEBGPU_BACKEND_DAWN` define requirement, the GLFW callback-chaining
  analysis** — all carried forward; re-confirmed independently below against the
  *current* `cpplib/platform.cpp` and `cpplib/graphics.cpp` (which didn't exist in
  final form when the M2 doc was written — main.cpp was a 2-line stub then, per that
  doc's own preamble).
- **One deviation from the M2 doc, decided here: do not vendor ImPlot for M4a.** See
  §3.3.

---

## 1. Dependency mechanism (FetchContent, backend files, Dawn accessor)

### 1.1 Pins (unchanged from M2 doc)

| Component | Pin | Files vendored |
|---|---|---|
| Dear ImGui | `v1.92.9`, commit `01380c579715e62fb9a8d6ec0502c4ea83bfde6e`, non-docking | `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui.h`, `imgui_internal.h`, `imstb_*.h`, `imconfig.h`, `backends/imgui_impl_glfw.{h,cpp}`, `backends/imgui_impl_wgpu.{h,cpp}` |
| ImPlot | **not vendored for M4a** — see §3.3 | — |

### 1.2 FetchContent snippet (`CMakeLists.txt`, inserted after the existing Dawn
`FetchContent_MakeAvailable(dawn)` at line 40, before `add_executable(polyphorm ...)`
at line 42)

```cmake
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui
  GIT_TAG        01380c579715e62fb9a8d6ec0502c4ea83bfde6e   # v1.92.9, non-docking
  GIT_SHALLOW    OFF   # shallow clone can't check out an arbitrary commit by hash
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
target_link_libraries(imgui PUBLIC webgpu_dawn webgpu_glfw glfw)
```

(`GIT_TAG` as a raw commit hash, not `v1.92.9`: guarantees the exact pin even if the
tag is ever force-moved upstream — belt-and-suspenders on top of "never floating."
`GIT_SHALLOW OFF` is required for hash-based `GIT_TAG`; CMake/git cannot shallow-fetch
an arbitrary commit that isn't a branch tip.)

Then, in `add_executable(polyphorm ...)`'s target block (`CMakeLists.txt:42-56`):

```cmake
target_link_libraries(polyphorm PRIVATE imgui webgpu_dawn webgpu_glfw glfw)
```

`cpplib/ui_stub.cpp` (`CMakeLists.txt:53`) is replaced by `cpplib/ui.cpp` (a *new* file —
the old D3D11-era `cpplib/ui.cpp` is reference-only and is not itself reused or
recompiled; see §3). None of the five test executables (`cpplib_tests`,
`file_system_tests`, `graphics_tests`, `shader_compile_tests`, `render_path_tests`,
`sim_kernel_tests`) link `cpplib/ui_stub.cpp` or its replacement today (confirmed:
`CMakeLists.txt:74-160`, none list a `ui*.cpp`) — so the `imgui` target is linked
**only** into `polyphorm`. `graphics.cpp` gains two small new functions for the render-
pass hook (§2) and format accessor (§1.3); neither references any imgui header, so no
imgui dependency leaks into the test binaries that also compile `graphics.cpp`. Minimal
surface, confirmed by grep of every `CMakeLists.txt` target.

### 1.3 Device/queue/format accessor — decide the graphics:: surface

`graphics.h:213` already exports `extern graphics::GraphicsContext *graphics_context;`
with `GraphicsContext{ wgpu::Device device; wgpu::Queue queue; GpuContext *gpu; }`
(`graphics.h:29-33`). **No new accessor is needed for device/queue** — `ui.cpp` can use
`graphics_context->device.Get()` / `graphics_context->queue.Get()` directly for
`ImGui_ImplWGPU_InitInfo::Device`.

What's missing is the render-target **format**. `graphics.cpp` hardcodes
`wgpu::TextureFormat::BGRA8UnormSrgb` as the window view format in two places
(`window_view()` at `graphics.cpp:158`, and `draw_mesh`'s pipeline cache key at
`graphics.cpp:703-705`) — there's no accessor exposing it. Add one function, matching
those call sites exactly so there's a single source of truth:

```cpp
// graphics.h, near get_render_target_window()
wgpu::TextureFormat get_window_surface_format();   // BGRA8UnormSrgb view format,
                                                    // matches window_view()/draw_mesh
```
```cpp
// graphics.cpp
wgpu::TextureFormat get_window_surface_format() { return wgpu::TextureFormat::BGRA8UnormSrgb; }
```

Used for `ImGui_ImplWGPU_InitInfo::RenderTargetFormat`. This is the *view* format
(matching what `window_view()` creates over the underlying `BGRA8Unorm` surface via
`viewFormats`, `gpu_context.cpp:125,133-134`), not `GpuContext::surface_format`
(`gpu_context.h:11`, `BGRA8Unorm`) — passing the latter would violate the "pass the
actual negotiated format" rule from the M2 doc's pitfall #4 and mismatch what the UI
pass actually renders into (§2).

---

## 2. Render-pass entry point (carryover #7)

### 2.1 What's actually available today

Read `cpplib/graphics.cpp` frame lifecycle in full. Key facts:

- `g_encoder` (`graphics.cpp:14`), `ensure_encoder()`/`flush_commands()`
  (`graphics.cpp:87-96`), and `window_view()` (`graphics.cpp:147-164`) are all `static` —
  file-private, not reachable from `ui.cpp` or `main.cpp`.
- `draw_mesh()` (`graphics.cpp:691-777`) opens and closes **its own** render pass per
  call, `LoadOp::Load` (`graphics.cpp:765`) — "preserve the prior clear/draw." This is
  the existing one-pass-per-call model; nothing in the codebase holds a render pass open
  across multiple draw calls.
- The encoder is **not** one continuous per-frame accumulation either:
  `update_constant_buffer`/`update_structured_buffer`/`run_clear` all force
  `flush_commands()` first if `g_encoder` is non-null (`graphics.cpp:280`, `308`, `486`)
  to preserve `WriteBuffer` ordering against already-recorded dispatches. `g_encoder` is
  flushed and recreated many times within a single frame already, by design. The single
  real "flush everything, then Present" point is `swap_frames()` (`graphics.cpp:192-199`),
  called once per frame at `main.cpp:1644`.

### 2.2 Decision: `graphics::begin_ui_pass()` / `graphics::end_ui_pass()`

This is exactly carryover #7's suggested shape, and it's the natural fit given §2.1: it
reuses `ensure_encoder()` + `window_view()` + a `LoadOp::Load` pass, identical in spirit
to what `draw_mesh()`/`clear_render_target()` already do, so it doesn't introduce a
second render-pass idiom into the codebase. The alternative — exposing `g_encoder`
itself, or a callback hook invoked from inside `swap_frames()` — was rejected: the
former breaks the "graphics.cpp owns all wgpu:: recording" encapsulation for no benefit
(ImGui's `RenderDrawData` only needs a pass, not the encoder), the latter would make
`graphics::` depend on `ui::` (a layering inversion; today only `ui::` → `graphics::`).

```cpp
// graphics.h
wgpu::RenderPassEncoder begin_ui_pass();          // window target, LoadOp::Load
void end_ui_pass(wgpu::RenderPassEncoder pass);   // pass.End()
```

```cpp
// graphics.cpp — reuses the existing static ensure_encoder()/window_view()
wgpu::RenderPassEncoder begin_ui_pass() {
    ensure_encoder();
    wgpu::RenderPassColorAttachment att = {};
    att.view = window_view();
    att.loadOp = wgpu::LoadOp::Load;
    att.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor pass_desc = {};
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &att;
    return g_encoder.BeginRenderPass(&pass_desc);
}
void end_ui_pass(wgpu::RenderPassEncoder pass) { pass.End(); }
```

`wgpu::RenderPassEncoder` is a refcounted `ObjectBase<Derived, CType>` handle (per the
M2 doc's Dawn-header read) — returning it by value is the same pattern already used for
`wgpu::Buffer` inside `Mesh`/`ConstantBuffer` (`graphics.h:74`, `79`), so this isn't a
new idiom for the codebase.

### 2.3 Call site: `ui::end()`

```cpp
// ui.cpp
void ui::end() {
    if (!g_frame_open) return;              // nothing pending (see §4.2)
    ImGui::Render();
    wgpu::RenderPassEncoder pass = graphics::begin_ui_pass();
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get());
    graphics::end_ui_pass(pass);
    g_frame_open = false;
}
```

Ordering within a frame: event drain (`main.cpp:843-850`) → sim → scene render
(`main.cpp:1160-1303`, all `draw_mesh`/`run_compute` calls against `g_encoder`) →
histogram readback + `ui::draw_rect`/`draw_text` (`main.cpp:1306-1457`) → parameter
panel (`main.cpp:1474-1639`) → `ui::end()` closes the frame → `graphics::swap_frames()`
(`main.cpp:1644`) does `flush_commands()` + `Present()`. `begin_ui_pass()`'s
`LoadOp::Load` means it draws on top of whatever the scene pass(es) already wrote to the
window view that frame — correct only because it's guaranteed to run *after* every scene
`draw_mesh` call and *before* `swap_frames()`. See §4 for why that ordering needs one
new line in `main.cpp`.

---

## 3. The `ui::` adapter

`cpplib/ui.h` (unchanged, 47 lines) stays the compiled contract. Read `cpplib/ui.cpp`
(720 lines, the deleted-pathway D3D11 original) and `cpplib/font.cpp` in full for
semantic reference — **neither file is compiled today** (`CMakeLists.txt:53` links only
`ui_stub.cpp`; grep of every target confirms no `font.cpp`/`ui.cpp` reference), they're
tree-resident documentation of "what did panels/sliders/draw_rect mean." The M4a
replacement is a **new** `cpplib/ui.cpp` implementing `ui.h` against ImGui, not a revival
of the old file.

### 3.1 Frame/widget model — a simplification over the original

The original `ui.cpp` buffers `TextItem`/`RectItem` into `Array<>`s during the frame and
flushes them in `ui::end()` (`ui.cpp:652-675`) — necessary there because the D3D11-era
renderer needed an explicit draw call per item at flush time. ImGui doesn't need this:
`ImGui::GetBackgroundDrawList()->AddRectFilled(...)`/`AddText(...)` already append into
an ImGui-owned list that's composited in `ImGui::Render()`, in call order, regardless of
when during the frame they're issued. **No buffering layer is needed in the M4a
adapter** — `draw_rect`/`draw_text` call directly into the draw list, immediately.

### 3.2 Function-by-function mapping

| `ui.h` function | Adapter behavior |
|---|---|
| `init(w, h)` | `ImGui::CreateContext()`; `ImGui_ImplGlfw_InitForOther(window, true)` (window handle must come from `platform::` — see §4.1 for the plumbing note); `ImGui_ImplWGPU_Init(&info)` with `info.Device = graphics_context->device.Get()`, `info.RenderTargetFormat = graphics::get_window_surface_format()` (§1.3), `info.DepthStencilFormat = WGPUTextureFormat_Undefined`, `info.NumFramesInFlight = 3`. `w`/`h` params become **vestigial** — ImGui derives `io.DisplaySize` live from GLFW every frame (§7), so no stored value can go stale the way the old `screen_width`/`screen_height` globals could (`ui.cpp:185,193-197`). Kept only so the signature is unchanged. |
| `draw_text(text, font*, x, y, color, origin)` (2 overloads) | `font*` param **ignored** — ImGui's font stack is global/current-font, not passed per-call; matches M2 doc's finding that main.cpp never calls these overloads with a non-default font anyway. Origin-relative positioning: compute `ImGui::CalcTextSize(text)`, offset by `-origin.x*w, -origin.y*h` (same formula as the original, `ui.cpp:277-278`), then `GetBackgroundDrawList()->AddText(pos, ImColor(color.x,color.y,color.z,color.w), text)`. |
| `draw_text(text, pos, color, origin)` | Same, calling the above with the default font. |
| `draw_rect(x, y, w, h, color)` (2 overloads) | `GetBackgroundDrawList()->AddRectFilled(ImVec2(x,y), ImVec2(x+w,y+h), ImColor(color.x,color.y,color.z,color.w))`. Coordinate convention check: old `draw_rect` took `y` top-relative internally (flips to `screen_height - y` only when building its own bottom-left NDC projection matrix, `ui.cpp:349`) — i.e. **callers already think in top-left, y-down pixel space**, exactly ImGui's `DrawList` convention. `main.cpp`'s histogram/eplot/trim-gizmo math (`histo_params`, `eplot_params`, `trim_params`, all built from `SCREEN_X`/`SCREEN_Y`) needs **zero changes** — every `(x,y,w,h,color)` call site ports mechanically, 1:1, no coordinate-math rewrite. `Vector4` color components are already `0..1` floats matching `ImColor`'s float constructor — confirmed by main.cpp's own literals (e.g. `Vector4(0.4, 0.9, 0.5, 0.5)`, `main.cpp:1346`) — no `/255` conversion needed anywhere. |
| `start_panel(name, pos, width)` (2 overloads) | `ImGui::Begin(*name ? name : "Polyphorm")`. **`pos`/`width` are not used to size/position the ImGui window** — see the quirk note below. Sets `g_frame_open` (lazy-inits the frame first if this is the first `ui::` call — see §4.2). Returns a `Panel` populated as before (unused by the adapter internally beyond `name`, kept for ABI parity since `Panel`'s fields are part of `ui.h`). |
| `end_panel(Panel*)` | `ImGui::End()`. |
| `get_panel_rect(Panel*)` | No call sites in `main.cpp` (confirmed, M2 doc's call-site table). Best-effort: `Vector4(ImGui::GetWindowPos().x, .y, ImGui::GetWindowSize().x, .y)` if called between `start_panel`/`end_panel`, else the old pass-through `Vector4(panel->pos.x, panel->pos.y, panel->width, 0)`. |
| `add_toggle(panel, label, state)` | `ImGui::Checkbox(label, state)` — return value **is** "changed this frame," identical contract to the original (`ui.cpp:476-555`, `changed` set only on the click edge). Direct 1:1. |
| `add_slider(panel, label, pos, min, max)` | `ImGui::SliderFloat(label, pos, min, max)` — same "changed this frame" contract. Direct 1:1. |
| `end()` | See §2.3 — idempotent, actually calls `ImGui::Render()` + records the UI pass, guarded by `g_frame_open`. |
| `release()` | `ImGui_ImplWGPU_Shutdown()`; `ImGui_ImplGlfw_Shutdown()`; `ImGui::DestroyContext()`. |
| `set_input_responsive(bool)` | `true`→clear `ImGuiConfigFlags_NoMouse` from `ImGui::GetIO().ConfigFlags`; `false`→set it (ImGui then ignores all mouse input/hit-testing, matching the old system's `is_input_rensposive_` gate on its own hand-rolled hit-testing, `ui.cpp:489,516,589,613`). Only call site is `main.cpp:470` with `true` — this branch is exercised at startup only today; implemented for completeness/API parity. |
| `is_input_responsive()` | `!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouse)`. |
| `is_registering_input()` | `ImGui::GetIO().WantCaptureMouse`. See §4.3 for why the *timing* of this call (before any widgets are built this frame) makes this the textbook-correct usage, not a hack. |
| `get_screen_width()` | `ImGui::GetIO().DisplaySize.x`. No call sites in `main.cpp` (M2 doc). |
| `get_font()` | Returns `nullptr`. `Font*` is a dead type on this path (`font.cpp`/`font.h` are uncompiled reference files, §3); no call site dereferences the result (M2 doc's grep). |

### 3.2.1 Quirk: `Panel`'s `width` parameter is unreliable — don't try to honor it

`main.cpp:1477`: `Panel panel = ui::start_panel("", Vector2(0.0, 0.0), 1.0);` — `width =
1.0`. In the old `ui.cpp`, this value feeds directly into the background rect's pixel
width (`get_panel_rect`, `ui.cpp:401-404`; `end_panel`, `ui.cpp:406-422`) with no
unit scaling — a literal `1.0`-pixel-wide background rectangle, evidently a latent
quirk in the D3D11-era code (the widgets themselves position independently via
`panel->item_pos`, so this only broke the *background* rect's visible width, not
layout). **Not something to "fix" while porting** (mechanical translation, not a
semantic rewrite — same principle as the `reset_pt` per-call-site contract below); the
ImGui adapter simply doesn't use `width`/`pos` for the window at all and lets
`ImGui::Begin` auto-size/auto-position (first call: ImGui's own default window position).
This is a **strictly better** outcome than reproducing the 1px-wide-background quirk,
and is presented as a deliberate, documented behavior change, not a silent bug import.

### 3.2.2 The `reset_pt` contract (M2 doc, unchanged, re-verified against current
`main.cpp`)

`main.cpp:1481-1634` is the exact same per-call-site pattern the M2 doc catalogued:
about a third of `add_slider` calls do **not** feed `reset_pt` (cosmetic ones — `OPTI
THICKNESS`, `BACKGROUND COL`, `OVERDENSITY LO/HI`, etc., e.g. `main.cpp:1535,1539,1566-
1567`) while most do (`reset_pt |= ui::add_slider(...)`, e.g. `main.cpp:1481`). Since
`ImGui::SliderFloat`/`Checkbox` return the identical "changed this frame" bool as
`ui::add_slider`/`add_toggle`, **no main.cpp line needs to change** — the existing `|=`
vs. plain-call pattern already encodes the right contract and requires no porting work
at all beyond `ui.h`'s signatures staying stable. Re-verified current line numbers
against the table above; unchanged from the M2 doc's finding.

### 3.3 Decision: do not vendor ImPlot for M4a

The M2 doc's Part 2 sketch defaulted to `ImPlot::PlotBars` for the density histogram and
energy plot. Re-examining against the *actual* current `main.cpp` code (read in full,
`main.cpp:1306-1457`): both plots are drawn as **individually pre-computed** `draw_rect`
calls in a loop — `main.cpp` itself already computes each bar's exact `x, y, width,
height` (`current_bar`, `main.cpp:1345`; `eplot_val`-derived rect, `main.cpp:1385-1390`)
and each label's exact position. There is no axis, no legend, no interactive zoom/pan —
it's a decorative HUD, not an interactive plot widget. Adopting `ImPlot::PlotBars`
would mean **restructuring** `main.cpp`'s histogram loop to hand ImPlot a data array and
let it own bar layout — a semantic rewrite of exactly the kind the `reset_pt` pitfall
(§3.2.2) warns against for sliders, now applied to the plot code. Mapping every
`draw_rect`/`draw_text` call directly onto `ImGui::GetBackgroundDrawList()` (§3.2) is a
**mechanical, call-site-preserving port** instead — zero main.cpp changes to the
histogram/eplot/trim-gizmo block (`main.cpp:1306-1457`) beyond the `ui::` calls already
resolving to something real.

Consequence: **ImPlot is dropped from the M4a vendor list** (§1.1) — one fewer
dependency, smaller pin surface, no `ImPlot::CreateContext()`/`DestroyContext()` lifetime
to manage. If a future milestone wants a real interactive plot (zoomable energy history,
etc.), that's a new, separate feature decision, not an M4a requirement — YAGNI. This is
a deliberate deviation from the M2 doc, flagged per this task's "carry forward" instruction
rather than silently dropped.

---

## 4. Input routing, frame lifecycle glue, and the one main.cpp line

### 4.1 GLFW callback order — re-verified against current `platform.cpp`

`cpplib/platform.cpp:40-74` (`platform::get_window`) installs `glfwSetKeyCallback`/
`glfwSetCursorPosCallback`/`glfwSetMouseButtonCallback`/`glfwSetScrollCallback` at window
creation and owns `glfwPollEvents()` (`platform.cpp:90`, inside `get_event`). `main.cpp`
calls `platform::get_window(...)` at `main.cpp:385`, and `ui::init(...)` only at
`main.cpp:469` — i.e. Polyphorm's own callbacks are already installed by the time
`ui::init()` would call `ImGui_ImplGlfw_InitForOther(window, /*install_callbacks=*/true)`.
Per the M2 doc's read of `imgui_impl_glfw.cpp`'s source, `install_callbacks=true` snapshots
the *existing* callbacks as `Prev*Callback` and chain-calls them unconditionally after
ImGui's own handler — order is correct, no change needed, re-confirmed against the
current file (this file didn't exist as anything but a stub when the M2 doc was written,
so this re-check is the one piece of "verify it still holds" with actual new information:
it does hold).

One plumbing gap: `platform::Window` (`platform.h`, not re-read in full here but implied
by `platform.cpp:49` `window.window_handle = handle;`) already carries the raw
`GLFWwindow*` — `ui::init(float, float)`'s signature has no `Window*`/`GLFWwindow*`
parameter, and per house rules `ui.h` cannot change. **`ui::init` needs the GLFW handle
some other way.** Two options: (a) widen `graphics::` to expose the window handle
alongside `graphics_context` (another accessor, more surface), or (b) have `ui.cpp`
reach it via a small new `platform::` accessor (e.g. `platform::get_glfw_window()`
returning the handle `platform.cpp:72`'s `g_glfw_window` already tracks — it's `static`
today, one-line change to expose). **Decision: (b)** — `platform.cpp` already owns
exactly one live `GLFWwindow*` in `g_glfw_window` (`platform.cpp:11`); exposing it is a
smaller, more logical surface addition than teaching `graphics::` about a GLFW type it
otherwise has no reason to know. Add:

```cpp
// platform.h
GLFWwindow *get_glfw_window();   // the window platform:: owns; needed by ui::init for ImGui_ImplGlfw_InitForOther
```

This is the one other small surface addition beyond `graphics::begin_ui_pass`/
`end_ui_pass`/`get_window_surface_format` (§1.3, §2.2) — `ui.h` itself stays untouched,
satisfying the house rule.

### 4.2 Frame lifecycle — why `ui::end()` must be idempotent, and the one new
`main.cpp` line

This is the least obvious part of the whole design and the biggest risk in this
document (see §8, risk #1). Re-read `main.cpp`'s actual control flow (not assumed —
walked line by line):

- `ui::is_registering_input()` (`main.cpp:854`) runs **unconditionally** every
  non-headless frame, before any other `ui::` call that frame (it's the first `ui::`
  touch after `ui::init`/`set_input_responsive` at startup). This is the natural place
  to lazily open the ImGui frame.
- `ui::end()` is called at **up to two, and sometimes zero,** places per frame:
  - `main.cpp:1457`, inside `if (compute_histogram) { if (!headless) { ... } }` —
    fires whenever `compute_histogram` is on (default: on, `main.cpp:813`).
  - `main.cpp:1638`, inside `if (!headless && show_ui) { ... ui::end_panel(&panel);
    ui::end(); }` — fires whenever `show_ui` is on (default: on, toggled by F1,
    `main.cpp:891`).
  - If **both** flags are on (the common case), `ui::end()` fires **twice** in the same
    frame — first at 1457, then again at 1638 (later in the same frame, so *that* call
    is the true "last `ui::` call before `swap_frames`").
  - If **both** flags are off, `ui::end()` never fires that frame at all — but
    `ImGui::NewFrame()` was still opened (via `is_registering_input()` at line 854).

`ImGui::NewFrame()` called twice without an intervening `ImGui::Render()`/`EndFrame()`
is an ImGui contract violation (its own internal assert). So the adapter must guarantee
**exactly one** `Render()`+submit per frame regardless of which of the four
`(compute_histogram, show_ui)` combinations is active that frame:

- `ui::end()` becomes **idempotent**: guarded by a `static bool g_frame_open` (set by
  the lazy-open helper, cleared once `Render()`+submit has run) — the second call in the
  "both on" case is a no-op (§2.3's `if (!g_frame_open) return;`).
- That alone doesn't cover the "both off" case — nothing calls `ui::end()` at all, so
  the frame's ImGui content (there is none, but the context is still open) never gets
  `Render()`-closed, and the *next* frame's lazy `NewFrame()` would hit ImGui's assert.
  **This needs one new line in `main.cpp`**, immediately before the existing
  `graphics::swap_frames()` call:

  ```cpp
  // main.cpp:1644, immediately before the existing line
  if (!headless) ui::end();      // NEW: guarantee frame closure even when
                                  // compute_histogram and show_ui are both off
  if (!headless) graphics::swap_frames();   // existing line, unchanged
  ```

  This calls the **already-existing** `ui::end()` (part of `ui.h`'s unchanged API) from
  one new call site — it does not add a function to `ui.h`, so the house rule ("ui.h API
  surface stays unchanged... main.cpp keeps compiling") is not violated: every *existing*
  call site compiles and behaves identically; this is one *additional*, defensive
  invocation of a function that already exists, directly analogous to the other
  `main.cpp` edits M4a is already scoped to make (carryover #6's `is_ready` gates, §5;
  the `graphics::` pre-fixes, §6).

Ordering is safe in all four cases: `ui::end()`'s idempotent guard means whichever call
runs first (1457 or 1638, or the new pre-`swap_frames` line if neither fired) does the
real `Render()`+`begin_ui_pass`/`RenderDrawData`/`end_ui_pass`, and it always happens
*after* the scene draw calls (`main.cpp:1160-1303`) and *before*
`swap_frames()`'s `Present()` (`main.cpp:1644`/new adjacent line) — exactly the ordering
`begin_ui_pass`'s `LoadOp::Load` requires (§2.3).

### 4.3 `is_registering_input()` timing is correct, not a hazard

`main.cpp:854`'s `if(!ui::is_registering_input())` gates camera orbit/pan/zoom, and is
evaluated *before* `start_panel`/`add_slider`/`add_toggle` are called for that frame
(those happen later, at `main.cpp:1477-1638`). Naively this looks like reading
`WantCaptureMouse` before the frame's widgets exist. It isn't a problem: this is
lazily where `ImGui::NewFrame()` itself gets called (§4.2), and `io.WantCaptureMouse` at
that point reflects ImGui's per-frame default computed from **last frame's** window
layout (window positions/sizes don't change until this frame's `Begin()` calls run) —
this is the textbook ImGui usage pattern (check `WantCaptureMouse` right after
`NewFrame()`, before your own game/camera input, exactly what `main.cpp:854` already
does structurally). Carrying forward the M2 doc's pitfall #2 (`WantCaptureMouse` is
broader than the old `active_id`-only gate — true while merely hovering, not just
dragging): still true, still a deliberate, documented behavior change (you can no longer
scroll-zoom while hovering an unclicked slider), not a bug.

---

## 5. Stub-branch gating (carryover #6)

`vis_mode` (`main.cpp:819`, default `VM_PARTICLES`) is switched by seven `ui::add_toggle`
calls in the panel (`main.cpp:1532,1543,1557,1574,1587,1599,1603`). Two of those
(`VIS: HALO COLOR` line 1574, `VIS: VELOCITY` line 1587) are compiled out by default
(`#ifdef HALO_COLOR_ANALYSIS`/`#ifdef VELOCITY_ANALYSIS`, `main.cpp:1572-1596`) — not
reachable in a default M4a build, not analyzed further here. The remaining five:

| Toggle (main.cpp) | `vis_mode` | Shader state | Reachable-safe today? |
|---|---|---|---|
| `main.cpp:1532` VIS: TRACE | `VM_VOLUME` | `vertex_shader`, `pixel_shader` both `= {}` (`main.cpp:522-523`) | **No** |
| `main.cpp:1543` VIS: HIGHLIGHTS | `VM_VOLUME_HIGHLIGHT` | `vertex_shader`, `ps_volume_highlight` both `= {}` | **No** |
| `main.cpp:1557` VIS: OVERDENSITY | `VM_VOLUME_OVERDENSITY` | `vertex_shader`, `ps_volume_overdensity` both `= {}` | **No** |
| `main.cpp:1599` VIS: PARTICLES | `VM_PARTICLES` | `vertex_shader_2d`, `pixel_shader_2d` real, compiled M3 | Yes (already shipped) |
| `main.cpp:1603` VIS: PATH TRACING | `VM_PATH_TRACING` | `cs_volpath`, `ps_volpath` both `= {}` | **No** |

`ui_stub.cpp`'s `add_toggle` always returns `false` and never mutates `*state`
(`ui_stub.cpp:19`) — that's *why* these branches are currently unreachable; the moment
`add_toggle` becomes a real `ImGui::Checkbox`, all four "No" rows become one click away
from `graphics::draw_mesh`'s `assert(g_vertex_shader && g_vertex_shader->valid && ...)`
(`graphics.cpp:692`), which is a **live, loud abort** in the app binary (`-UNDEBUG`,
`CMakeLists.txt:63`), not a silent no-op.

### Decision: gate, don't land shaders (M4a defers real volume/PT shaders to M4b)

Per this task's scope (M4a = UI plumbing; M4b = the actual volume/PT shader port),
pick the "gate every branch on `is_ready`" horn of carryover #6, not "land the M4
shaders with the UI." Concretely:

- **`VM_VOLUME` / `VM_VOLUME_HIGHLIGHT` / `VM_VOLUME_OVERDENSITY`** (`main.cpp:1202-
  1262`): the three `draw_mesh(&super_quad_mesh)` calls (`main.cpp:1245,1251,1257`) are
  shared across all three sub-modes (only the bound pixel shader differs, selected at
  `main.cpp:1210-1236`). Track which `PixelShader*` was selected in that if/else-if
  chain (a local `PixelShader *selected_ps` set alongside each `graphics::set_pixel_shader`
  call, `main.cpp:1211,1216,1224,1232,1235`), and gate the "draw the stack" block
  (`main.cpp:1238-1258`) on `graphics::is_ready(&vertex_shader) && selected_ps &&
  graphics::is_ready(selected_ps)`. When not ready: skip the block entirely — the window
  already shows `clear_render_target`'s `background_color` fill from `main.cpp:1163`
  (executed unconditionally before the `vis_mode` switch), i.e. a safe blank/background
  frame, not a crash.
- **`VM_PATH_TRACING`** (`main.cpp:1263-1302`): the *compute* half is already safely
  gated — `graphics::is_ready(&cs_volpath)` is checked at `main.cpp:1269` before
  dispatching. The **draw** half is not: `main.cpp:1296-1300`
  (`set_vertex_shader(&vertex_shader_2d)` — real/valid; `set_pixel_shader(&ps_volpath)` —
  stub; `draw_mesh(&quad_mesh)`) runs unconditionally whenever `vis_mode ==
  VM_PATH_TRACING`, regardless of `cs_volpath`'s readiness, and hits the same
  `draw_mesh` assert via `ps_volpath`. Gate: wrap `main.cpp:1296-1301` in `if
  (graphics::is_ready(&ps_volpath)) { ... }`. When not ready: skip — again, a blank
  background-colored frame, not a crash. (`display_tex`, the texture that block would
  sample, is also never written without a working `cs_volpath` dispatch, so even if the
  draw *weren't* gated on `ps_volpath` specifically, the visual result would still be
  meaningless/stale — gating on shader readiness is the right signal, not a proxy.)

### Why not disable the checkbox instead?

`ui::add_toggle`'s signature (`ui.h:33`) takes no "enabled/disabled" parameter, and
widening it would violate the house rule that `ui.h`'s surface stays unchanged. Per
YAGNI: leave all five toggles clickable, let the not-ready branches no-op to a blank
frame. This is a materially different (and simpler) choice than trying to gray out
specific checkboxes, and the M2 doc's font/text-optionality reasoning applies by
analogy — visual "coming soon" affordance is a nice-to-have, not required for the M4a
bar (real sliders/toggles/histogram/energy plot working). A future milestone that wants
grayed-out toggles can widen `ui.h` deliberately then, with its own cost/benefit call.

---

## 6. Task-zero: `graphics::` pre-fixes M4a should land first

Per this task's scope (the exact set named: "I1 sampler clear + main.cpp:1280 typo
adjudication, compute slot-pair fix, topology cache key, comment fixes") — land these
*before* wiring up the ImGui adapter, since the adapter's own draw calls (§2, §3) are one
more thing exercising the render-slot bind-group machinery these fixes touch, and a
clean baseline makes it easier to attribute any bring-up failures to the new UI code
rather than a pre-existing bug.

1. **I1a — sampler-slot leakage.** `graphics.cpp:552`:
   `void unset_texture(uint32_t slot) { assert(slot < MAX_SLOTS); g_render_slots[slot].view = nullptr; }`
   only clears `.view`; add `g_render_slots[slot].sampler = nullptr;`. The adjacent
   comment (`graphics.cpp:550-551`, "samplers are never unset by main.cpp... they stay
   bound") documents the *current* (soon-to-be-wrong) contract and needs updating in the
   same change. Safe today only because every draw in the shipped M3 binary reuses the
   same slot/sampler set every frame (VM_PARTICLES only); becomes load-bearing the
   moment a second render-path shader variant with a different binding set is exercised
   — which M4a's newly-reachable volume branches would be, if not for the gating in §5.
   Land it now regardless, since it's cheap and removes a latent trap for M4b.
2. **I1b — `main.cpp:1280` typo adjudication.**
   `graphics::set_texture_sampler(&tex_sampler_deposit, 2);` sits inside
   `VM_PATH_TRACING`'s **compute**-bind block (`main.cpp:1270-1284` — every sibling call
   in that block is a `_compute` variant: `set_texture_compute`,
   `set_texture_sampled_compute`, `set_texture_sampler_compute`). This one line calls the
   **render**-path's `set_texture_sampler`, writing into `g_render_slots[2]`
   (`graphics.cpp:553`) instead of `g_compute_slots[2]`. Cross-read against `draw_mesh`'s
   bind-group-1 construction (`graphics.cpp:743-755`): any slot with a non-null `.view`
   **or** `.sampler` gets a bind-group entry — so once this line runs, `g_render_slots[2]`
   permanently carries a sampler-only entry that every *subsequent* `draw_mesh` call that
   frame (and onward, since nothing clears samplers pre-fix-#1) would try to bind at
   `@binding(5)` (`2*2+1`), a hard Dawn validation error against any pixel shader that
   doesn't declare that binding (e.g. `ps_particles_color`, VM_PARTICLES' shader, which
   only declares slot-0 bindings) — this is carryover #1's "permanently poisons render
   slot 2" claim, confirmed here by reading the actual bind-group-construction code.
   Almost certainly should be `graphics::set_texture_sampler_compute(&tex_sampler_deposit, 2);`
   to pair with the `set_texture_sampled_compute(&trail_tex_A/B, 2)` calls at
   `main.cpp:1276,1278`. Currently masked because `VM_PATH_TRACING`'s compute dispatch
   never runs (`cs_volpath` invalid, §5) — but §5's gating only skips the *draw* half at
   1296-1300, not the bind-setup at 1270-1284, which still executes every frame
   `vis_mode == VM_PATH_TRACING` regardless of `cs_volpath` readiness (the `is_ready`
   check at `main.cpp:1269` only guards the `run_compute` dispatch itself, not the
   preceding `set_texture_*` calls) — so this typo is **already live and reachable**
   the moment the `VIS: PATH TRACING` toggle works, independent of M4b. Must fix in
   M4a, not defer.
3. **Compute-side texture+sampler same-slot collision.** `set_texture_sampled_compute`
   (`graphics.cpp:557-558`) and `set_texture_sampler_compute` (`graphics.cpp:559`) both
   start with `g_compute_slots[slot] = {}` — a single-`Kind` tagged union
   (`graphics.cpp:20-26`), unlike the render path's `{view, sampler}` pair
   (`graphics.cpp:41`). `cs_volpath`'s intended bind layout pairs texture+sampler at the
   *same* slot numbers 1, 3, 4 (`main.cpp:1273-1274,1281-1282,1283-1284`:
   `set_texture_sampled_compute(&trace_tex, 1)` then `set_texture_sampler_compute(&tex_sampler_trace, 1)`,
   etc.) — the second call's `{} ` erases the first call's `.view`. Currently inert
   (dispatch never runs, `cs_volpath` invalid) but must be fixed **before** M4b lands the
   real `cs_volpath` shader, and this task's scope explicitly names it as an M4a
   pre-fix. Adopt the render path's `{view, sampler}`-pair-per-slot shape for
   `g_compute_slots` too (or the 2N/2N+1 slot-doubling convention already used on the
   render side, `graphics.cpp:540-547`) — either resolves the collision; picking the
   pair-struct (matching `RenderSlot`) is the smaller diff since it reuses an existing
   pattern in the same file.
4. **Topology cache key.** Small fix from `m3-carryovers.md`'s fix-in-M4 list: fold
   `topology` into `PipelineCacheEntry` (`graphics.cpp:63-69`) rather than leaving it as
   a comment-only caveat — `TRIANGLESTRIP` is currently unused so this is latent, land
   it while touching this file for the other fixes above.
5. **Comment fixes.** `draw_mesh`'s group-1 CONTRACT comment (`graphics.cpp:540-547`)
   needs rewriting once fix #1 lands (it currently documents the wrong "samplers are
   never unset" contract); `graphics.h:19`'s stale `display_tex_uint`-deleted comment
   (referenced by `m3-carryovers.md`'s small-fixes list) — not independently re-verified
   here beyond the carryover's own note, low-risk text-only fix.

Explicitly **out of scope for this task-zero list** (per this task's own scoping,
narrower than everything `m3-carryovers.md` lists): the missing
`set_constant_buffer(&rendering_settings_buffer, 0)` before the three volume
`draw_mesh` calls (carryover #3) — moot while those branches stay gated-off by §5, real
work for M4b when the volume shaders actually land; the blend alpha-channel formula
guess (carryover #4) — unobservable until M4b's shaders vary alpha per-fragment; the
28-byte stride coverage gap (carryover #5) — unrelated to UI. Not landing these now is a
deliberate scope cut, not an oversight.

---

## 7. Retina / `DisplayFramebufferScale`

Read `cpplib/gpu/gpu_context.cpp:114-137` (`init_surface`) in full. The surface is
configured at **framebuffer pixel** resolution (`glfwGetFramebufferSize`,
`gpu_context.cpp:120-123`, "on Retina displays the framebuffer is 2x logical... leaves
the swapchain at half resolution" if logical size were used instead) — this is the M3
"Option A" tradeoff the human visual gate checklist already adjudicated (blocky 2x
nearest-neighbor upscale is expected, not a bug, per `m3-carryovers.md`'s checklist item
5). Meanwhile `main.cpp`'s own pixel-space math for the histogram/eplot/trim-gizmo
(`histo_params`, `eplot_params`, `trim_params`, `main.cpp:1338-1442`) is built from
`SCREEN_X`/`SCREEN_Y` — the **logical**, `config.polyp`-specified window size
(`main.cpp:369-382`), i.e. `window_width`/`window_height`, *not* the framebuffer pixel
dimensions `gpu_context.cpp` actually configures the surface at. (`display_tex`,
`display_accum_buffer`, and `rendering_config.screen_width/height` are also sized from
this same logical `window_width`/`window_height`, `main.cpp:543,656,732-733` — the
"blocky upscale" is the *scene*'s low-res buffer stretched to fill the full-res
framebuffer-native surface at draw time.)

This matters for the ImGui mapping because it turns out to compose cleanly, with **no
manual scaling code needed anywhere**:

- `ImGui_ImplGlfw_NewFrame()` sets `io.DisplaySize` from `glfwGetWindowSize` (**logical**
  points) and `io.DisplayFramebufferScale` from the ratio to `glfwGetFramebufferSize`
  (**pixels**) — confirmed in the M2 doc's direct read of the backend source, not
  re-fetched here since the backend file itself hasn't changed (pinned by commit hash).
- `ImGui::GetBackgroundDrawList()`'s `AddRectFilled`/`AddText` (§3.2) take coordinates in
  the **same units as `io.DisplaySize`** — i.e. **logical points**, exactly the
  `SCREEN_X`/`SCREEN_Y`-relative coordinate space `main.cpp`'s `histo_params` etc.
  already use. The `x, y, w, h` values `draw_rect`/`draw_text` call sites pass port
  **unchanged** (§3.2's table) — they happen to already be in the right coordinate
  system for ImGui's DrawList API, purely because both systems chose "logical/points,
  top-left origin, y-down" independently.
  `ImGui_ImplWGPU_RenderDrawData` multiplies by `DisplayFramebufferScale` internally when
  building GPU vertex data, so the *rendered* result lands at full framebuffer-pixel
  resolution against `graphics::get_window_surface_format()`'s target (§1.3, §2.2) —
  crisp on Retina, without `ui.cpp` doing any scale math itself.
- Net effect: the ImGui overlay renders crisp/native-res on Retina, while the 3D scene
  underneath stays at the already-adjudicated blocky-upscaled logical resolution — a
  visible resolution *mismatch* between overlay and scene, but that's the pre-existing,
  already-signed-off M3 tradeoff, not something M4a introduces or needs to reconcile.
  The M2 doc's pitfall #6 (written before `graphics.cpp`'s real framebuffer-native
  surface config existed) worried about exactly this kind of mismatch in the abstract;
  now that both sides of it are concretely known, it resolves itself with zero
  additional code — downgraded from "pitfall to watch" to "verified, no action needed."

---

## 8. Risk register (ordered)

1. **Frame lifecycle: double/zero `ui::end()` calls per frame (§4.2).** Highest risk —
   subtle, not caught by a compiler, and the failure mode (ImGui's internal
   `NewFrame()`-without-`Render()` assert, or a double-`Render()` assert) would abort the
   app on the very first frame where `compute_histogram`/`show_ui` combine in the
   "wrong" way, which is actually the *default* startup state (both true, main.cpp:813,
   805). Mitigation: idempotent `ui::end()` (§2.3) + the one added `main.cpp` line before
   `swap_frames()` (§4.2) covering the "both off" case. This is the one piece of this
   design most worth writing a targeted smoke test for before trusting it (e.g. toggle
   `show_ui`/`compute_histogram` via F1 and the panel checkbox across several frames in
   the headless or windowed test path).
2. **Stub-branch reachability (carryover #6, §5).** Second-highest — a live `assert`
   abort, one click away, the moment `add_toggle` stops being a no-op. Mitigation:
   `is_ready` gating on the four affected `draw_mesh`/draw-half call sites, enumerated
   with exact `main.cpp` line ranges in §5. Verify by clicking every VIS: toggle in a
   manual smoke pass once M4a lands — this is the natural human-verification step
   analogous to M3's visual gate.
3. **`main.cpp:1280` render/compute sampler-slot typo (§6, item 2).** Already
   independently live (runs whenever `VM_PATH_TRACING` is selected, regardless of
   `cs_volpath` readiness) — not gated by §5's fix, since it's in the bind-setup code
   that precedes the gated compute dispatch. If missed, the *next* `draw_mesh` call
   after selecting Path Tracing (e.g. switching back to Particles) would hit a Dawn
   validation error from the poisoned slot. Mitigation: task-zero fix #2, must land
   before or alongside the UI wiring, not after.
4. **Compute slot-pair collision (§6, item 3).** Currently inert (dispatch never runs)
   but becomes load-bearing the instant M4b lands `cs_volpath` for real; landing the fix
   now (as this task's scope requires) removes it as a risk for that future milestone
   rather than this one — low risk *to M4a itself*, included because the task named it
   explicitly.
5. **`RenderTargetFormat` mismatch (§1.3).** Low risk given the single-accessor design
   (`graphics::get_window_surface_format()`) — the failure mode (wrong gamma, or a hard
   pipeline-format-mismatch validation error) is well-understood and the M2 doc already
   verified the backend's format-driven gamma correction is automatic once the *right*
   format is passed in. Mitigation is structural (one function, one call site), not
   procedural.
6. **Single-panel adapter assumption (§3.2).** The adapter maps `Panel*` loosely onto
   ImGui's implicit current-window stack rather than tracking an explicit
   `ImGuiWindow*` per `Panel` — correct today because `main.cpp` only ever has one
   `start_panel`/`end_panel` bracket open at a time (verified: exactly one call site,
   `main.cpp:1477-1637`). Low risk *now*; flagged so a future change that opens a second
   concurrent panel doesn't silently misbehave (widgets landing in the wrong window) —
   would need revisiting at that point, not preemptively solved (YAGNI).
7. **`platform::get_glfw_window()` accessor (§4.1).** Small, mechanical surface
   addition (`static` → exported getter) — essentially zero risk, listed for
   completeness since it's the one place this design touches a file outside
   `ui.*`/`graphics.*`.
8. **Retina/`DisplayFramebufferScale` (§7).** Assessed and resolved during this
   research, not a residual risk — included in the register only to record that it was
   checked, per the task's explicit design question, and that the M2 doc's pitfall #6
   downgrades to "no action needed" now that `graphics.cpp`'s real surface-config code
   exists to check it against.

---

## Summary of every surface addition beyond `ui.h` (which stays unchanged)

| File | Addition | Why |
|---|---|---|
| `graphics.h`/`graphics.cpp` | `wgpu::TextureFormat get_window_surface_format()` | Single source of truth for the imgui backend's `RenderTargetFormat` (§1.3) |
| `graphics.h`/`graphics.cpp` | `wgpu::RenderPassEncoder begin_ui_pass()`, `void end_ui_pass(wgpu::RenderPassEncoder)` | The render-pass entry point carryover #7 asked for (§2.2) |
| `platform.h`/`platform.cpp` | `GLFWwindow *get_glfw_window()` | `ui::init` needs the handle for `ImGui_ImplGlfw_InitForOther`; `platform.cpp` already tracks it in `g_glfw_window` (§4.1) |
| `main.cpp:1644` (immediately before the existing `swap_frames()` line) | `if (!headless) ui::end();` | Guarantees exactly one `ImGui::Render()`+submit per frame even when `compute_histogram` and `show_ui` are both off (§4.2) |
| `main.cpp` (multiple, enumerated in §5/§6) | `is_ready` gates around volume/PT `draw_mesh` calls; the `main.cpp:1280` typo fix; the compute slot-pair fix in `graphics.cpp` | Carryover #6 + task-zero pre-fixes |
| `cpplib/ui.cpp` (new file, replaces `ui_stub.cpp` in the `polyphorm` target) | Full `ui.h` implementation against ImGui | The actual deliverable |
| `CMakeLists.txt` | `imgui` `FetchContent`/static-lib target, linked into `polyphorm` only | §1.2 |

`ui.h` itself: **zero changes.**
