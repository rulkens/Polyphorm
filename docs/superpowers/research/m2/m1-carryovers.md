# M1 → M2 carryovers

Decisions and obligations from the M1 final whole-branch review and parallel
research that the M2 plan must pick up. M1 closed at `f7c2296` (+ research
commit `663a300`).

## Must land in M2

- **Device limit gates in `gpu::init`** (spec: Error handling). Defaults are
  `maxComputeInvocationsPerWorkgroup = 256` — below the original shaders'
  `numthreads(10,10,10)` = 1000 and `(8,8,8)` = 512 — and
  `maxBufferSize` 256 MiB / `maxStorageBufferBindingSize` 128 MiB, far below
  the multi-GB grids. Request raised limits in the `DeviceDescriptor`
  (single choke point) and fatal-error naming the limit when unavailable,
  same pattern as the existing `float32-filterable` gate.
  Fallback if a limit can't be raised: workgroup reshape per
  `wgsl-drafts/translation-notes.md` (bitwise-identical for decay,
  statistically identical for agents).
- **Dawn error routing through `logging`**: `gpu_context.cpp` callbacks are
  fprintf-only because `logging.cpp` isn't compiled yet; M2 enables it
  (`# enabled in M2` markers in CMakeLists.txt) and reroutes. Add
  `[[noreturn]]` to `fatal()` while there.
- **Window resizability decision**: M1 forces `GLFW_RESIZABLE = FALSE`; the
  Win32 original was resizable (`WS_OVERLAPPEDWINDOW`). If M2 restores it,
  surface-reconfigure-on-resize belongs in the `graphics::` layer.

## Conventions established in M1

- **Framebuffer vs logical size**: the WebGPU surface is configured at
  framebuffer (Retina 2x) resolution; `Window::window_width/height` stay
  logical by design — M2 mouse/camera code works in logical space and must
  not conflate the two.
- Build: incremental `cmake -B build` only — never wipe `build/` (Dawn
  re-download). `nice -n 19 cmake --build build -j 8`, never bare `-j`.
  Clean-configure verification recipe (zero download):
  `cmake -B <dir> -DFETCHCONTENT_SOURCE_DIR_DAWN=$PWD/build/_deps/dawn-src`.

## Key research facts (details in sibling docs)

- `graphics-api-inventory.md`: 39 used / ~20 dead `graphics::` functions;
  5 call sites reach through `.ua_view` for clear-UAV (no WebGPU primitive —
  needs explicit design); per-frame histogram readback must become
  staging-buffer `mapAsync` (likely one frame of latency).
- `imgui-integration.md`: pin ImGui v1.92.9 + ImPlot v1.0; backends verified
  against Dawn v20260807.193620; install order = platform callbacks first,
  ImGui second.
- `wgsl-drafts/`: shaders live in `shaders/` (not `bin/` as the spec's table
  implies); shipped `REGIME_SDSS` deposit/trace textures are single-channel
  `R16_FLOAT` (main.cpp:568/574), so `r32float` is exact, not lossy; quirk
  toggles via pipeline-overridable bool constants (17 across two drafts);
  RNG-consuming toggles keep draws inside the branch to avoid stream desync.
- Upstream oddity (not core scope): PT dispatch binds its slot-2 sampler to
  the wrong pipeline stage — note in quirk registry if the volpath ever
  returns.

## Human verification note

Esc/close-button live exit was verified by code review + unit tests only
(no synthetic-input permission in agent sandbox); a manual 30-second run
was requested from the user at M1 close.
