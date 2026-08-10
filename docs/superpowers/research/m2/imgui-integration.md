# Dear ImGui + ImPlot integration research (M2/M4 UI replacement)

Repo: `/Users/rulkens/Development/vendor/cpp/Polyphorm`, branch `macos-webgpu-port`.
Context: `main.cpp` is still the pre-port, bug-for-bug-faithful scientific core (per
`docs/superpowers/specs/2026-08-10-macos-webgpu-port-design.md`); `graphics.*`/`ui.*`/
`font.*` are the D3D11-era files still to be ported/deleted (`CMakeLists.txt` comments
mark `graphics.cpp`, `memory.cpp`, `random.cpp`, `logging.cpp`, `file_system.cpp` as
"uncommented in Task 3/4"). `main_m1.cpp` is a 2-line stub; the real WebGPU `main.cpp`
does not exist yet on this branch. This research targets the UI slice of that future
port (M4 per the design doc: "trace volume mode + ImGui parameter panel + plots").

---

## Part 1 — repo inventory

### `cpplib/ui.h` public API (47 lines, full contents read)

```cpp
namespace ui {
    void init(float screen_width, float screen_height);
    void draw_text(const char *text, Font *font, float x, float y, Vector4 color, Vector2 origin = Vector2(0,0));
    void draw_text(const char *text, Font *font, Vector2 pos, Vector4 color, Vector2 origin = Vector2(0,0));
    void draw_text(const char *text, Vector2 pos, Vector4 color, Vector2 origin = Vector2(0,0));
    void draw_rect(float x, float y, float width, float height, Vector4 color);
    void draw_rect(Vector2 pos, float width, float height, Vector4 color);
    Panel start_panel(char *name, Vector2 pos, float width);
    Panel start_panel(char *name, float x, float y, float width);
    void end_panel(Panel *panel);
    Vector4 get_panel_rect(Panel *panel);
    void end();
    bool add_toggle(Panel *panel, char *label, bool *state);
    bool add_slider(Panel *panel, char *label, float *pos, float min, float max);
    void release();
    void set_input_responsive(bool is_responsive);
    bool is_input_responsive();
    bool is_registering_input();
    float get_screen_width();
    Font *get_font();
}
```

`ui.cpp` (720 lines, read in full) is an immediate-mode retained-per-frame widget system:
draw calls are buffered into `Array<TextItem>`/`Array<RectItem>` during the frame and
flushed by `ui::end()`; hit-testing uses a hot/active id model (`hash_string(label)` as
the id — **labels are the widget identity**, exactly like ImGui's `##id` / label-as-id
convention, which makes the 1:1 mapping to `ImGui::SliderFloat(label, ...)` /
`ImGui::Checkbox(label, ...)` direct). `is_registering_input()` becomes true while a
slider/toggle is `active` (mouse captured) and gates `main.cpp`'s own camera-drag input
handling (`if(!ui::is_registering_input()) { ... mouse orbit/scroll ... }`, line 876) —
this is the input-capture contract that must be replicated with `ImGuiIO::WantCaptureMouse`
(see Pitfalls).

### Every `ui::` call site in `main.cpp` (1670 lines total)

Setup/tail (outside the per-frame loop):
| Line | Call | Notes |
|---|---|---|
| 442 | `ui::init((float)window_width, (float)window_height)` | after `font::init()` (441) and `graphics::init_swap_chain` |
| 443 | `ui::set_input_responsive(true)` | |
| 876 | `if(!ui::is_registering_input())` | gates camera orbit/pan/zoom input; **must preserve**: replace with `!ImGuiIO.WantCaptureMouse` (see pitfalls — semantics differ slightly) |
| 1629 | `ui::release()` | shutdown |

Per-frame overlay drawing (histogram/energy-plot/trim-cube gizmo, lines 1296–1441, inside
`if (compute_histogram)`) — pure `draw_rect`/`draw_text` calls, **no return values used**:
lines 1334, 1344, 1368, 1375, 1385, 1393, 1401, 1408, 1420, 1431, 1432, 1434, 1435, 1437,
1438, then `ui::end()` at 1440.

Parameter panel (lines 1456–1621, inside `if (show_ui)`) — this is the block whose
returns matter:

| Line(s) | Call | Return feeds `reset_pt`? |
|---|---|---|
| 1459 | `Panel panel = ui::start_panel("", Vector2(0,0), 1.0)` | — |
| 1463 | `add_slider(&panel,"SENSE ANGLE [DEG]",&ss,0,90)` | **yes** (`reset_pt \|=`) |
| 1466 | `add_slider("SENSE DIST [MPC]",&sd_mpc,0,10)` | **yes** |
| 1469 | `add_slider("MOVE ANGLE [DEG]",&ts,0,45)` | **yes** |
| 1472 | `add_slider("MOVE DIST [MPC]",&md_mpc,0,1)` | **yes** |
| 1474 | `add_slider("AGENT DEPOSIT",&simulation_config.deposit_value,0,10)` | **yes** |
| 1475 | `add_slider("PERSISTENCE",&simulation_config.decay_factor,0.8,0.995)` | **yes** |
| 1477 | `add_slider("SAMPLING EXP",&simulation_config.move_sense_coef,0.0001,10)` | **yes** |
| 1480 | `add_slider("TRACE WEIGHT",&swgt,-5,3)` | **yes** |
| 1482 | `add_slider("DEPOSIT WEIGHT",&rendering_config.galaxy_weight,0,1)` | **yes** |
| 1485 | `add_toggle("TRACE HISTOGRAM",&compute_histogram)` | no (return discarded) |
| 1487 | `add_toggle("HIST RNG SAMPLING",&random_histogram_sampling)` | no |
| 1490 | `add_toggle("VOLUME TRIMMING",&do_trimming)` | no |
| 1494,1495 | `add_slider("X POS"/"X WIDTH",&trim_pos,&trim_width,0,1)` (×3 for X/Y/Z, lines 1494-1507) | **yes**, all 6 |
| 1514 | `add_toggle("VIS: TRACE",&is_toggled)` | no — but `is_toggled` gates `vis_mode` switch, and downstream sliders in that branch (1517–1521) are NOT or-ed into `reset_pt` except TRIM DENSITY (1519, yes) |
| 1517,1521 | `add_slider("OPTI THICKNESS"...)`, `add_slider("BACKGROUND COL"...)` | **no** (plain call, no `\|=`) |
| 1519 | `add_slider("TRIM DENSITY",&trd,-5,9)` | **yes** |
| 1525–1535 | VIS: HIGHLIGHTS branch — same pattern: OPTI THICKNESS/HGLGHT DENSITY/BACKGROUND COL not tracked, TRIM DENSITY (1533) tracked | mixed |
| 1539–1551 | VIS: OVERDENSITY branch — TRIM DENSITY (1544) tracked, OVERDENSITY LO/HI (1548,1549) **not** tracked | mixed |
| 1554–1565 | `#ifdef HALO_COLOR_ANALYSIS` VIS: HALO COLOR branch, same pattern | mixed, compiled out by default |
| 1567–1578 | `#ifdef VELOCITY_ANALYSIS` VIS: VELOCITY branch, same pattern | mixed, compiled out by default |
| 1581 | `add_toggle("VIS: PARTICLES",&is_toggled)` | no |
| 1585–1616 | VIS: PATH TRACING branch: SIGMA_T/ALBEDO/SIGMA_E/ANISOTROPY/AMBI TRCE/N BOUNCES/TRACE_MAX (1590–1602) all **yes**; EXPOSURE (1608/1610) conditionally tracked only if `compressive_accumulation` is on; COMPRESSIVE EXPOSURE toggle (1614) **yes** (toggle return tracked here, unusually); RENDER! toggle (1616) **no** |
| 1619 | `ui::end_panel(&panel)` | — |
| 1620 | `ui::end()` | — |

**Contract to preserve**: it is NOT "every slider resets path tracing" — about a third of
sliders (cosmetic ones: optical thickness, background color, overdensity thresholds) are
deliberately excluded from `reset_pt`. When porting to `ImGui::SliderFloat`/`Checkbox`
(which return `bool` "value changed this frame" identically to `ui::add_slider`/
`add_toggle`), the *same subset* of call sites must be OR'd into `reset_pt` — this is a
line-by-line mechanical translation, not a semantic rewrite. `ui::add_toggle`'s return
is used at only one site (1614, COMPRESSIVE EXPOSURE) despite being called ~10 times;
elsewhere the toggle's mutated `*state` bool is read directly instead.

### Energy plot + density histogram (lines 1296–1441)

- **Density histogram**: `density_histogram` is a `unsigned int[N_HISTOGRAM_BINS]` (17
  bins, `main.cpp:190`) read back every frame via
  `graphics::capture_structured_buffer(&density_histogram_buffer, density_histogram, ...)`
  (1300) from a GPU storage buffer written by `cs_density_histo.hlsl`. `main.cpp` computes
  `mean`/`variance`/`energy` CPU-side from it (1301–1312), then draws 16 bars
  (`N_HISTOGRAM_BINS-1`, bin 0 is a "null" bin) via `ui::draw_rect` in a loop (1327–1348),
  each bar height = `histo_params2.x * count[b]/norm_coef`. **Update cadence: every
  frame**, no throttling (unlike the energy plot).
- **Energy plot**: `eplot_vals[100]` (`eplot_res = 100`, line 823) is a ring buffer;
  `eplot_ptr` advances one slot only every `1000/100 = 10` frames when `run_mold` is true
  (line 1355, `frame_num % int(1.0e3/eplot_res) == 0`) — i.e. it samples `mean` (the
  same histogram-derived scalar, called `objective_value`) roughly every 10 frames, not
  every frame. Drawn as 100 thin bars (`ui::draw_rect`, 1366–1374) plus one "E" label
  glyph. `max_objective` is a running max (never reset) used to normalize bar height.
  This ring-buffer/decimated-sampling pattern maps directly onto `ImPlot::PlotBars` fed
  from a persistent `float eplot_vals[100]` (or `ImPlotBuffer`); no changes needed to the
  sampling logic, only the draw call.
- Both plots + a 3D trim-box gizmo (1414–1438, also `draw_rect`/`draw_text`) are drawn
  into `render_target_window` (1317) **before** the parameter panel block and outside the
  `show_ui` toggle — i.e. histogram/energy-plot/trim-gizmo show whenever
  `compute_histogram` is on, independent of `show_ui` (F1). Preserve this independence:
  don't gate the plot windows behind the same ImGui window/flag as the slider panel
  unless intentionally changing behavior.

### Font/text usage (font.* being deleted)

- `ui.cpp` owns its own `Font font_ui`, loaded once in `ui::init()` from
  `renner-book.otf` via `font::get(...)` (line 233) — **not** the app's `font::init()`
  call at `main.cpp:441`, which is a separate, apparently otherwise-unused global font
  system (grep found no other call site consuming `font::init()`'s result in `main.cpp`;
  it exists only to satisfy `font.h`'s global state before `ui::init` pulls its own copy).
  Both should be deletable together — `ui::draw_text`'s 3-arg overload (`draw_text(text,
  pos, color, origin)`) always uses the internal `font_ui`, and `main.cpp` never calls
  the `Font*`-taking overloads with anything other than the default (grep confirms no
  `ui::get_font()` call sites in `main.cpp`).
- All text in `main.cpp` is short numeric/label strings (histogram bin indices, "E: %.4f",
  "M: %.2f", "null: %.2f%%", "(log %g)", axis labels "X"/"Y", slider value labels via
  `ui::add_slider`'s internal `sprintf_s(..., "%.2f", *pos)`) — nothing requiring custom
  glyph ranges, Unicode, or a specific font family. **ImGui's built-in proggy-clean
  default font (no external font file) is a drop-in replacement**; no need to port
  `renner-book.otf` loading unless the team wants to keep the aesthetic (ImGui supports
  loading arbitrary TTF/OTF via `ImFontAtlas::AddFontFromFileTTF`, so `renner-book.otf`
  could be kept and loaded through ImGui's font system if desired — optional, not
  required for functional parity).

---

## Part 2 — integration research

### Dawn pin already in this repo

`CMakeLists.txt:36`: `GIT_TAG v20260807.193620` (dated tag scheme, Dawn abandoned
`chromium/NNNN` branches). Verified against the actually-fetched/built source
(`build/_deps/dawn-build/gen/include/dawn/webgpu.h`, commit `c23537c0`, tag date
2026-08-07):

- Uses the **current webgpu-headers API generation**: `WGPUStringView` (not `const
  char*`) for all labels/messages/shader source; `WGPURequestAdapterCallbackInfo`
  Future-based async APIs (not the old bare-callback `wgpuInstanceRequestAdapter`);
  `WGPUSurfaceConfiguration`/`WGPUSurfaceCapabilities`/`wgpuSurfaceConfigure` (the
  **Surface** API — no `WGPUSwapChain` type exists in this header at all, it's fully
  gone, not just deprecated).
- `webgpu_cpp.h` (generated, at `build/_deps/dawn-build/gen/include/dawn/webgpu_cpp.h`)
  wraps every handle in an `ObjectBase<Derived, CType>` with a `.Get()` accessor and an
  implicit (`explicit(false)`) `CType`-constructor — so `wgpu::Device` and `WGPUDevice`
  convert both directions for free, which is how the imgui backend (raw-C API) will
  interop with Polyphorm's future `graphics.*` (likely `webgpu_cpp.h`-based, matching
  `webgpu_glfw` link target already in `CMakeLists.txt:54`).

### `imgui_impl_wgpu` backend — verdict: compatible, current

Checked the actual backend source at tag `v1.92.9` (not summarized — fetched and
`grep`'d directly, plus cross-checked GitHub's tags API for real dates since a prior
`WebFetch` summarization pass returned fabricated/wrong dates that contradicted the
file's own changelog — flagging so any downstream reader trusts the API-verified facts
below over prose summaries of rendered pages):

- Already speaks **`WGPUStringView`** natively (`{ "Dear ImGui Vertex buffer", WGPU_STRLEN }`
  literal-init pattern throughout) and the **Surface API** (has
  `ImGui_ImplWGPU_CreateWGPUSurfaceHelper()`, no `WGPUSwapChain` reference anywhere).
- Changelog entries confirm active Dawn tracking through 2026: `2026-04-23` (draw
  callbacks), `2026-03-25` (WGVK backend), `2026-03-09` (dropped Emscripten <4.0.10),
  `2025-10-16` ("Update to compile with Dawn and Emscripten's 4.0.10+ ports"),
  `2025-02-26` ("Update for latest webgpu-native changes"), `2024-10-14` ("Update Dawn
  support for change of string usages") — i.e. the `WGPUStringView` migration this repo's
  Dawn pin requires was absorbed **two years before** this Dawn tag was cut; no gap.
- Requires exactly one of `IMGUI_IMPL_WEBGPU_BACKEND_DAWN` / `_WGPU` / `_WGVK` defined
  (compile error otherwise) — set `IMGUI_IMPL_WEBGPU_BACKEND_DAWN`. This define changes
  one thing in the backend: `using WGPUProgrammableStageDescriptor = WGPUComputeState;`
  (Dawn renamed the compute-stage descriptor type; the alias papers over it) — confirmed
  present in Dawn's generated header (`WGPUComputeState` exists, `WGPUProgrammableStageDescriptor`
  does not, in `build/_deps/dawn-build/gen/include/dawn/webgpu.h`).
- **sRGB handling is automatic**: the backend's fragment shader has a `Gamma` uniform;
  `ImGui_ImplWGPU_Init`/texture-update code checks `RenderTargetFormat` against a switch
  over every `*Srgb` `WGPUTextureFormat` enumerator (incl. `BGRA8UnormSrgb`, the format
  macOS surfaces commonly report) and sets `Gamma = 2.2` vs `1.0` accordingly — **as
  long as `ImGui_ImplWGPU_InitInfo::RenderTargetFormat` is set to the actual negotiated
  surface/view format**, no manual gamma correction is needed. Passing the wrong format
  (e.g. hardcoding a non-sRGB format while the surface actually gives you
  `BGRA8UnormSrgb`) is the failure mode to avoid, not a missing feature.

### `imgui_impl_glfw` — coexistence with Polyphorm's own GLFW callbacks: compatible

Polyphorm's `cpplib/platform.cpp` (133 lines, read in full) already installs its own
`glfwSetKeyCallback`/`glfwSetCursorPosCallback`/`glfwSetMouseButtonCallback`/
`glfwSetScrollCallback` at window-creation time (`platform::get_window`, lines 42–72),
feeding a private ring-buffer event queue that `platform::get_event` drains, and calls
`glfwPollEvents()` itself once per frame (line 90, inside the same function that also
polls `glfwWindowShouldClose`).

- `ImGui_ImplGlfw_InitForOther(window, /*install_callbacks=*/true)` called **after**
  `platform::get_window()` will call `ImGui_ImplGlfw_InstallCallbacks()`, which snapshots
  Polyphorm's existing callbacks as `Prev*Callback` and **chain-calls them** after its own
  handler runs (confirmed in source: `bd->PrevUserCallbackKey(...)` etc. called
  unconditionally after ImGui's own logic, for all 7 callback types). Order matters:
  Polyphorm's callback install must happen first (it already does — `get_window` runs
  long before any UI init would).
- `ImGui_ImplGlfw_NewFrame()` does **not** call `glfwPollEvents()` itself — it only reads
  window/framebuffer size and cursor state — so Polyphorm continuing to own the
  `glfwPollEvents()` call is correct and requires no change.
- DPI: `ImGui_ImplGlfw_GetWindowSizeAndFramebufferScale` sets `io.DisplaySize` from
  `glfwGetWindowSize` (points) and `io.DisplayFramebufferScale` from the ratio to
  `glfwGetFramebufferSize` (pixels) — standard, automatic Retina handling on the ImGui
  side. See Pitfalls for why Polyphorm's own pixel-space UI math needs a matching check.

### ImPlot — compatible, pin the tagged release

- `implot.h`'s own header comment currently says `// ImPlot v1.1 WIP` on `master`
  (last commit `2026-06-08`), but the last **tagged** release is `v1.0`
  (`524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39`, tagged `2026-04-05` — real dates, verified
  via GitHub's tags/commits REST API, not page-summarization). Maintainership: recent
  copyright header is `Copyright (c) 2020-2024 Evan Pezent` + `Copyright (c) 2025-2026
  Breno Cunha Queiroz` — actively maintained under a new maintainer, not abandoned.
- Compatibility check done directly, not assumed: ImPlot `v1.0`'s `implot.cpp` gates
  newer-ImGui-only code behind `#if IMGUI_VERSION_NUM < NNNNN` blocks, highest guard
  found is `19173`. This repo's chosen imgui `v1.92.9` reports `IMGUI_VERSION_NUM 19290`
  — above every guard in ImPlot v1.0, so ImPlot takes the "modern API" branch everywhere
  and needs no patching. **No unreleased ImPlot commits are required.**
- Files to vendor: `implot.h`, `implot_internal.h`, `implot.cpp`, `implot_items.cpp`
  (`implot_demo.cpp` optional, demo-only). No files beyond these four are load-bearing.
- Init beyond `ImGui::CreateContext()`: only `ImPlot::CreateContext()` /
  `ImPlot::DestroyContext()` at matching points to ImGui's context lifetime — no
  renderer-specific ImPlot init; it draws through the same ImGui draw-list/backend, so
  once `imgui_impl_wgpu`+`imgui_impl_glfw` work, ImPlot needs nothing WebGPU-specific.

### Recommended pins

| Component | Pin | Why |
|---|---|---|
| Dear ImGui | tag **`v1.92.9`** (commit `01380c579715e62fb9a8d6ec0502c4ea83bfde6e`), **non-docking** `master` line | Newest tagged release (2026-07-25, verified via API); WebGPU backend at this tag natively speaks `WGPUStringView`+Surface API matching this repo's Dawn pin with a 2-year compatibility margin. Non-docking (not `v1.92.9-docking`) because Polyphorm needs one single-viewport window with an overlay panel + 2 plots — no multi-window/docking requirement, and non-docking is the simpler/lower-risk tree (docking is a long-lived separate branch with its own multi-viewport platform-window plumbing that would add GLFW-backend complexity for no win here). |
| ImPlot | tag **`v1.0`** (commit `524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39`) | Latest tagged release; verified compatible with imgui 1.92.9's `IMGUI_VERSION_NUM` (19290, above every guard ImPlot v1.0 checks for); actively maintained fork, no reason to track WIP `master`. |
| Dawn | already pinned, unchanged | `v20260807.193620` (`CMakeLists.txt:36`), confirmed compatible with the imgui pin above — do not touch. |

### Exact file list to vendor

```
third_party/imgui/                     (imgui v1.92.9, non-docking)
  imgui.h
  imgui.cpp
  imgui_draw.cpp
  imgui_tables.cpp
  imgui_widgets.cpp
  imgui_internal.h
  imstb_rectpack.h
  imstb_textedit.h
  imstb_truetype.h
  imconfig.h                           (edit: nothing required; or add IMGUI_IMPL_WEBGPU_BACKEND_DAWN here instead of a CMake define)
  imgui_demo.cpp                       (optional — drop if binary size matters, safe to omit)
  backends/
    imgui_impl_glfw.h
    imgui_impl_glfw.cpp
    imgui_impl_wgpu.h
    imgui_impl_wgpu.cpp

third_party/implot/                    (ImPlot v1.0)
  implot.h
  implot_internal.h
  implot.cpp
  implot_items.cpp
  implot_demo.cpp                      (optional, omit)
```

CMake sketch (new target or folded into the `polyphorm` executable — matching the single-
target style already in `CMakeLists.txt`):

```cmake
add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_glfw.cpp
  third_party/imgui/backends/imgui_impl_wgpu.cpp
  third_party/implot/implot.cpp
  third_party/implot/implot_items.cpp
)
target_include_directories(imgui PUBLIC
  third_party/imgui
  third_party/imgui/backends
  third_party/implot
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
target_link_libraries(imgui PUBLIC webgpu_dawn webgpu_glfw glfw)

target_link_libraries(polyphorm PRIVATE imgui)
```

### Init / frame / shutdown code sketch (against Dawn's `webgpu_cpp.h`)

Assumes a future `graphics.*` exposes (or main.cpp holds directly) a `wgpu::Device
device`, `wgpu::Surface surface`, and the negotiated `wgpu::TextureFormat
surface_format` from `surface.GetCapabilities(adapter, &caps)` — mirroring the
`WGPUSurfaceCapabilities`/`WGPUSurfaceConfiguration` API already present in this repo's
Dawn build (checked above; no `WGPUSwapChain` fallback exists to reach for instead).

```cpp
// --- init (once, after platform::get_window + Dawn device/surface bring-up) ---
IMGUI_CHECKVERSION();
ImGui::CreateContext();
ImPlot::CreateContext();
ImGuiIO &io = ImGui::GetIO();
io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
ImGui::StyleColorsDark();

ImGui_ImplGlfw_InitForOther(window.window_handle, /*install_callbacks=*/true);

ImGui_ImplWGPU_InitInfo init_info = {};
init_info.Device = device.Get();                       // wgpu::Device -> WGPUDevice
init_info.NumFramesInFlight = 3;
init_info.RenderTargetFormat = static_cast<WGPUTextureFormat>(surface_format);
init_info.DepthStencilFormat = WGPUTextureFormat_Undefined; // UI drawn in its own pass, no depth
ImGui_ImplWGPU_Init(&init_info);

// --- per frame ---
ImGui_ImplWGPU_NewFrame();
ImGui_ImplGlfw_NewFrame();
ImGui::NewFrame();

// ... build ImGui::Begin("Polyphorm")/SliderFloat/Checkbox calls translated 1:1 from
//     the ui:: call-site table above, same reset_pt |= pattern ...
// ... ImPlot::BeginPlot("Energy")/PlotLine(eplot_vals, ...)/EndPlot() ...
// ... ImPlot::BeginPlot("Density histogram")/PlotBars(density_histogram, ...)/EndPlot() ...

ImGui::Render();

// after the 3D scene render pass ends (present-target texture view already acquired):
wgpu::RenderPassColorAttachment ui_color_attachment = {};
ui_color_attachment.view = surface_view;                // same swapchain/surface texture view as the 3D pass
ui_color_attachment.loadOp = wgpu::LoadOp::Load;         // keep the rendered scene, overlay on top
ui_color_attachment.storeOp = wgpu::StoreOp::Store;
wgpu::RenderPassDescriptor ui_pass_desc = {};
ui_pass_desc.colorAttachmentCount = 1;
ui_pass_desc.colorAttachments = &ui_color_attachment;
// no depthStencilAttachment — matches DepthStencilFormat = Undefined above

wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
wgpu::RenderPassEncoder ui_pass = encoder.BeginRenderPass(&ui_pass_desc);
ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), ui_pass.Get());
ui_pass.End();
wgpu::CommandBuffer cmds = encoder.Finish();
queue.Submit(1, &cmds);

// --- shutdown ---
ImGui_ImplWGPU_Shutdown();
ImGui_ImplGlfw_Shutdown();
ImPlot::DestroyContext();
ImGui::DestroyContext();
```

---

## Pitfalls

1. **`reset_pt` contract is per-call-site, not per-widget-type.** About a third of the
   ~35 slider calls deliberately do *not* feed `reset_pt` (cosmetic sliders: optical
   thickness, background color, overdensity thresholds; see the call-site table above).
   A naive "every `SliderFloat` return ORs into `reset_pt`" port is wrong — copy the
   `|=` vs plain-call pattern line-by-line from the table.

2. **`ui::is_registering_input()` vs `ImGuiIO::WantCaptureMouse` are not quite the same
   gate.** The old system only reports "true" while a slider/toggle is *actively being
   dragged/clicked* (`active_id != -1`); merely hovering a widget does not set it.
   `WantCaptureMouse` is broader — true whenever the mouse is over any ImGui window,
   hovering a widget, or an item is active. Because Polyphorm's panel is drawn
   full-width across the top of the screen (`ui::start_panel("", Vector2(0,0), 1.0)`,
   effectively covering much of the viewport width), using `WantCaptureMouse` directly
   should still block camera-orbit while over the panel, which is arguably *more*
   correct than the old behavior (old: you could scroll-zoom while hovering a slider you
   weren't dragging) — but call this out as a deliberate behavior change, not a bug, when
   porting line 876.

3. **Render pass split, not a shared pass with the 3D scene.** Don't try to draw ImGui
   inside the same render pass as the volume-trace/particle pass. Init
   `DepthStencilFormat = Undefined` and give ImGui its own `LoadOp::Load` pass afterward
   (sketch above) — simplest, avoids depth-format/sample-count mismatches between the 3D
   pipeline and ImGui's internally-created pipeline (which is built once from
   `init_info.RenderTargetFormat`/`DepthStencilFormat`/`PipelineMultisampleState` and
   will assert/validate-error if a later render pass it's asked to draw into doesn't
   match).

4. **sRGB/format mismatch is a "pass the real format" bug, not a missing backend
   feature.** `RenderTargetFormat` must be the *actual* negotiated surface format
   (`surface.GetCapabilities(...)`), typically `BGRA8UnormSrgb` on macOS/Metal. The
   backend auto-corrects gamma based on that format (confirmed in source, see above) —
   the failure mode is hardcoding a format that doesn't match what the surface actually
   gives you at present time, not needing extra gamma code.

5. **`IMGUI_IMPL_WEBGPU_BACKEND_DAWN` must be defined for every TU that includes
   `imgui_impl_wgpu.h`** (it's a compile-time branch on struct-name aliasing, checked at
   preprocess time with a hard `#error` otherwise) — put it in `imconfig.h` or as a
   `target_compile_definitions(... PUBLIC ...)` on the vendored `imgui` library target so
   it propagates to `main.cpp` too if `main.cpp` ever directly references WGPU types
   alongside ImGui calls.

6. **DPI/Retina: Polyphorm's existing pixel-space UI math (now being replaced) assumed
   window size == framebuffer size** (`ui::init((float)window_width, (float)window_height)`
   using the raw `SCREEN_X`/`SCREEN_Y` from `config.polyp` as both the GLFW window size
   *and* the pixel dimensions fed into `rendering_config.screen_width/height` for shader
   math). On a Retina display, `glfwCreateWindow(SCREEN_X, SCREEN_Y, ...)` creates a
   window that is `SCREEN_X`×`SCREEN_Y` *points*, but the actual framebuffer (and thus
   the render targets/viewport the volume-trace shaders reason about in pixels) is
   `SCREEN_X*scale`×`SCREEN_Y*scale`. This is a pre-existing consideration for the whole
   port (not introduced by ImGui), but it directly affects the UI layer too: ImGui's
   `io.DisplaySize` is in points while `io.DisplayFramebufferScale` carries the Retina
   factor, and `ImGui_ImplWGPU_RenderDrawData` multiplies by that scale internally — so
   the ImGui panel will render crisp/correctly-scaled automatically *regardless* of
   whatever convention `graphics.*` ends up choosing for the 3D render targets. Just
   don't assume the two systems (ImGui overlay vs. Polyphorm's own `rendering_config
   .screen_width/height`) are using the same points-vs-pixels convention without
   checking once `graphics.*` is ported — a mismatch would show as the ImGui panel and
   the histogram-gizmo-in-shader-space (if any pixel math survives in WGSL) being scaled
   differently relative to each other.

7. **`imgui_demo.cpp`/`implot_demo.cpp` are safe to omit** from the vendored tree — pure
   demo code, not referenced by anything else, no reason to compile them into the shipped
   binary.

8. **Font**: no external font file is load-bearing for functional parity (see Part 1) —
   ImGui's built-in default font is sufficient; treat porting `renner-book.otf` through
   `ImFontAtlas::AddFontFromFileTTF` as a cosmetic nice-to-have, not required for the M4
   milestone's "parameter panel + plots" bar.
