# M4a — ImGui GUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The GUI is real — Dear ImGui replaces the ui:: stub so the user can drive the simulation live (parameter sliders, vis-mode toggles, histogram + energy plot), with the graphics-layer traps fixed first and every not-yet-ported vis mode failing safely to a blank frame.

**Architecture:** Follow `docs/superpowers/research/m4/imgui-integration-design.md` (**DESIGN**) exactly — it resolves every question with file:line evidence. Task 1 lands DESIGN §6's task-zero graphics fixes (+ the compute-sampler binding scheme this plan pins down). Task 2 vendors pinned ImGui, writes the new `cpplib/ui.cpp` adapter (DESIGN §3's function table), adds the three small accessors, and applies the main.cpp glue (idempotent-`ui::end` defensive call, §5's is_ready gates). Task 3 is the human interaction gate.

**Tech Stack:** Dear ImGui v1.92.9 (commit 01380c579715e62fb9a8d6ec0502c4ea83bfde6e, no ImPlot — DESIGN §3.3), imgui_impl_glfw + imgui_impl_wgpu (Dawn backend), C++17, CMake/ctest.

## Global Constraints

- `cpplib/ui.h` stays byte-unchanged; main.cpp's ~50 ui:: call sites keep compiling unmodified (the ONLY main.cpp edits are the ones DESIGN's summary table enumerates: the defensive `ui::end()` line, the §5 is_ready gates, the §6 typo fix).
- ImGui pinned by commit hash, `GIT_SHALLOW OFF`, `IMGUI_IMPL_WEBGPU_BACKEND_DAWN` defined; linked into the `polyphorm` target ONLY — no imgui dependency may leak into any test binary.
- Compute binding convention amendment (pinned by THIS plan; M4b's cs_volpath port will match it): non-sampler resources keep `binding == slot` (all shipped shaders unchanged); compute SAMPLERS bind at `@group(1) @binding(MAX_SLOTS + slot)` (= 16 + N). Document at the definition site and in the run_compute CONTRACT comment.
- Render convention unchanged: texture@2N / sampler@2N+1.
- Build incrementally ONLY: `cmake -B build`, `nice -n 19 cmake --build build -j 8`. NEVER wipe `build/` (Dawn re-download), NEVER bare `-j`. Note: the ImGui FetchContent adds a small (~100MB, full-history) clone on first configure — expected, one-time.
- All ctest suites green before every commit (energy_smoke must not regress; headless path never initializes ImGui).
- After ANY manual `./build/polyphorm` run: kill the process if it didn't exit; leave NO instances running.
- Quirk discipline: mechanical ports, no behavior "fixes" beyond the ones DESIGN explicitly adjudicates (the Panel-width 1px quirk is deliberately NOT reproduced — DESIGN §3.2.1; WantCaptureMouse being broader than the old gate is a documented behavior change — DESIGN §4.3).
- End every commit message with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

## File Structure

- Task 1: `cpplib/graphics.cpp` (+`graphics.h` comment), `main.cpp` (typo fix only), `tests/render_path_tests.cpp`, `tests/graphics_tests.cpp` or a new compute-sampler test
- Task 2: `CMakeLists.txt`, `cpplib/ui.cpp` (NEW — replaces ui_stub.cpp in the target; ui_stub.cpp stays on disk), `cpplib/graphics.{h,cpp}` (3 small functions), `cpplib/platform.{h,cpp}` (1 getter), `main.cpp` (defensive ui::end + §5 gates)

---

### Task 1: Graphics task-zero fixes (DESIGN §6)

**Files:**
- Modify: `cpplib/graphics.cpp`, `cpplib/graphics.h` (comments/struct only), `main.cpp` (one line: the 1280 typo)
- Test: `tests/render_path_tests.cpp` (extend), `tests/graphics_tests.cpp` (extend)

**Interfaces:**
- Produces: `unset_texture` clears BOTH view and sampler; `g_compute_slots` becomes pair-shaped `{BoundSlot resource; wgpu::Sampler sampler;}` (or equivalent — resource part keeps its existing Kind machinery) with samplers binding at `16 + slot`; pipeline cache key includes topology; corrected CONTRACT comments. Task 2's UI draws and M4b's cs_volpath port rely on these exact semantics.

- [ ] **Step 1: I1a — unset_texture clears sampler** (DESIGN §6.1). Add `g_render_slots[slot].sampler = nullptr;`; rewrite the adjacent stale comment.

- [ ] **Step 2: I1b — main.cpp:1280 typo** (DESIGN §6.2). `graphics::set_texture_sampler(&tex_sampler_deposit, 2);` inside VM_PATH_TRACING's compute-bind block becomes `graphics::set_texture_sampler_compute(&tex_sampler_deposit, 2);` with a `// upstream typo: render-stage call in compute bind block — see m3/m4 carryovers` comment.

- [ ] **Step 3: compute sampler pairing** (DESIGN §6.3 + this plan's binding scheme). Reshape compute shadow state so a sampled texture and a sampler coexist at the same slot: keep the existing per-slot resource storage exactly as-is (Kind machinery, `binding == slot`) and add independent per-slot sampler storage (`wgpu::Sampler`). `set_texture_sampler_compute(sampler, slot)` writes ONLY the sampler field (no longer zeroes the resource); `set_texture_sampled_compute` and the other setters write ONLY the resource part; unsets clear their own part; since graphics.h has no dedicated compute-sampler-unset function (check the header — its surface stays unchanged), make `unset_texture_sampled_compute(slot)` clear BOTH the resource and the sampler fields of that slot (mirroring I1a's render-side rationale: no caller separates them). run_compute's bind-group-1 builder emits resource entries at `binding == slot` (unchanged) plus sampler entries at `binding == 16 + slot`. Document: `// Compute samplers bind at MAX_SLOTS + slot: resources keep binding==slot so all pre-M4 shaders are unchanged; cs_volpath's WGSL (M4b) declares @binding(17/19/20) for its s1/s3/s4 samplers.`

- [ ] **Step 4: topology into the pipeline cache key** (DESIGN §6.4) + CONTRACT comment rewrites (DESIGN §6.5) + `graphics.h:19` stale-comment fix.

- [ ] **Step 5: Tests.**
  - render_path_tests: after the existing draw test, bind an extra sampler at an unused slot, `unset_texture` that slot, draw again — must not Dawn-error (pins I1a; before the fix a phantom sampler entry would break layout match. Construct so it genuinely fails pre-fix: set BOTH view+sampler at slot 1, unset_texture(1), draw with the slot-0-only shader).
  - graphics_tests (or a new test in render_path_tests using compute): a minimal WGSL kernel declaring `texture_2d<f32>` at `@binding(1)`, `sampler` at `@binding(17)`, sampling into a storage buffer at `@binding(0)`; bind via `set_texture_sampled_compute(&tex, 1)` + `set_texture_sampler_compute(&samp, 1)` + `set_structured_buffer(&out, 0)`; dispatch; read back; assert the sampled value. This pins the pairing fix AND the 16+N binding scheme end-to-end (it would fail before the fix: the sampler call zeroed the texture).

- [ ] **Step 6: Build + all suites + commit.** `nice -n 19 cmake --build build -j 8 && ctest --test-dir build` — all green.

```bash
git add cpplib/graphics.h cpplib/graphics.cpp main.cpp tests/
git commit -m "graphics: task-zero fixes — sampler unset, compute sampler pairing (16+N), 1280 typo, topology key"
```

---

### Task 2: ImGui vendoring + ui.cpp adapter + glue

**Files:**
- Modify: `CMakeLists.txt` (DESIGN §1.2 snippet verbatim; swap `cpplib/ui_stub.cpp` → `cpplib/ui.cpp` in the polyphorm target)
- Create: `cpplib/ui.cpp` (the adapter — DESIGN §3.2's function table is the spec, §2.3's ui::end body, §4.2's g_frame_open lifecycle)
- Modify: `cpplib/graphics.h` + `graphics.cpp` (`get_window_surface_format`, `begin_ui_pass`, `end_ui_pass` — DESIGN §1.3/§2.2 code verbatim)
- Modify: `cpplib/platform.h` + `platform.cpp` (`GLFWwindow *get_glfw_window()` — DESIGN §4.1)
- Modify: `main.cpp` (exactly two kinds of edits: the defensive `if (!headless) ui::end();` before swap_frames — DESIGN §4.2; the §5 is_ready gates on the three volume draw blocks + the PT draw half)

**Interfaces:**
- Consumes: Task 1's fixed graphics layer.
- Produces: a windowed polyphorm with a live ImGui panel. Headless path untouched (ui::init/end never called headless — verify the guards; energy_smoke green).

- [ ] **Step 1: CMake.** DESIGN §1.2 verbatim (FetchContent pin, static lib, `IMGUI_IMPL_WEBGPU_BACKEND_DAWN`, link into polyphorm only; swap ui_stub.cpp → ui.cpp in the target source list — leave ui_stub.cpp on disk). Configure + build the imgui lib alone first to surface fetch/compile issues early.

- [ ] **Step 2: Accessors.** graphics: `get_window_surface_format()` returning `BGRA8UnormSrgb` (single source of truth — add a comment at window_view() pointing at it), `begin_ui_pass()`/`end_ui_pass()` per DESIGN §2.2's code. platform: export the GLFWwindow getter.

- [ ] **Step 3: cpplib/ui.cpp.** Implement every ui.h function per DESIGN §3.2's table — that table IS the spec (init via `ImGui_ImplGlfw_InitForOther(platform::get_glfw_window(), true)` + `ImGui_ImplWGPU_Init` with device/queue from `graphics_context` and format from the accessor; lazy frame-open helper called from the first per-frame ui:: touch (is_registering_input / start_panel / draw_rect / draw_text each lazy-open); idempotent end() with g_frame_open; draw_rect/draw_text onto GetBackgroundDrawList with the origin offset formula; Checkbox/SliderFloat direct; WantCaptureMouse for is_registering_input; NoMouse flag for set_input_responsive; release() shuts down both backends + context). Headless safety: ui:: is never called when headless (main.cpp guards exist) — additionally make init() a no-op if `graphics_context` is null, defensive.

- [ ] **Step 4: main.cpp glue.**
  - Immediately before the `if (!headless) graphics::swap_frames();` line: add `if (!headless) ui::end();   // guarantee exactly one ImGui Render per frame (design §4.2)`.
  - §5 gates: in the VM_VOLUME* block, track `PixelShader *selected_ps` through the if/else-if chain and gate the draw-stack block on `graphics::is_ready(&vertex_shader) && selected_ps && graphics::is_ready(selected_ps)`; wrap VM_PATH_TRACING's draw half (set vs/ps + draw_mesh + unset) in `if (graphics::is_ready(&ps_volpath))`. Comment each gate `// M4b: real shader lands, gate stays as belt-and-suspenders`.

- [ ] **Step 5: Build + suites + windowed smoke.** All ctest suites green (none link imgui — verify by building tests before polyphorm if needed). Then windowed run from a dataset dir: expect the panel to appear with sliders/toggles, histogram bars + energy plot overlaying the particle view. Exercise programmatically what you can from logs (no Dawn errors across minutes; F-keys via... you cannot inject input — just verify no aborts over a few minutes of uptime and clean exit). Kill the process. If ImGui asserts on frame lifecycle, DESIGN §4.2 is the debug map.

- [ ] **Step 6: Commit.**

```bash
git add CMakeLists.txt cpplib/ui.cpp cpplib/graphics.h cpplib/graphics.cpp cpplib/platform.h cpplib/platform.cpp main.cpp
git commit -m "ui: real Dear ImGui adapter — pinned vendor, ui pass hook, frame-lifecycle glue, stub-branch gates"
```

---

### Task 3 (human gate): interaction verification

Coordinator asks the human to run windowed (chain dataset recommended: `/tmp/polyviz2`) and check:
1. Panel visible with sliders (SENSE/MOVE/PERSISTENCE/SAMPLING EXP...) and toggles; histogram bars + red energy plot overlay bottom-left; trim gizmo bottom-right.
2. Dragging PERSISTENCE down makes structure dissolve; back up, it re-forms (live sim control — the actual point of M4a).
3. Mouse over the panel does NOT orbit the camera; mouse off-panel does (WantCaptureMouse gating). Note: hovering a slider now blocks scroll-zoom — documented behavior change, not a bug.
4. VIS: TRACE / HIGHLIGHTS / OVERDENSITY / PATH TRACING toggles → blank background-colored frame, NO crash (gates working); VIS: PARTICLES returns to the particle view; after visiting PATH TRACING and returning, particles still render (pins the 1280-typo fix).
5. F1 hides/shows the panel; histogram keeps drawing when panel hidden (independent flags); Esc exits clean, no leftover process.
6. ImGui overlay is crisp on Retina while the scene stays soft (expected — DESIGN §7).

---

## Deferred to M4b (explicit)

- Volume + PT shader ports (vs_3d, ps_volume_trace/highlight/overdensity, cs_volpath+ps_volpath), palette TGA loading, per-draw `set_constant_buffer(&rendering_settings_buffer, 0)` for volume draws, blend alpha screenshot-compare, 28B stride test, save_texture3D (M5-critical — still standing).
- cs_volpath WGSL must declare compute samplers at `@binding(16 + slot)` per Task 1's scheme.
