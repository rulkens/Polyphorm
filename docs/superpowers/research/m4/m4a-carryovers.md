# M4a → M4b carryovers

From the M4a final whole-branch review (READY WITH FIXES, 4b90e8d..a098681).
M4a closed with the real Dear ImGui GUI live (panel, sliders, toggles,
histogram + energy overlay), a resizable window, and the graphics-layer
task-zero fixes landed. Human interaction gate: PASSED 2026-08-12 after the
one-cycle-per-frame interaction fix (6ba5b9d) — user confirmed live sim
control ("ok ui works again"). Final-review fixes landed as a098681.

## Binding contract for the cs_volpath port (M4b task 1 reading material)

- Compute samplers bind at `@group(1) @binding(MAX_SLOTS + slot)` = 16+N;
  non-sampler resources keep `binding == slot`.
- cs_volpath declares FOUR samplers: **s1/s2/s3/s4 → @binding(17/18/19/20)**.
  (The M4a plan's Global Constraints and early comments said 17/19/20 —
  that list was WRONG, corrected in a098681; it described upstream's buggy
  bind calls, not the HLSL declarations. s2 = tex_deposit_sampler is
  actively used at cs_volpath.hlsl:183.)
- The PT bind block now unsets slot 4 after dispatch (a098681) — upstream
  omission, same adjudication class as the :1280 typo. Without it the
  leaked palette sampler at binding 20 fails strict-match on the next
  frame's sim kernels.
- Frame-lifecycle contract: ONE ImGui NewFrame/Render cycle per app frame;
  flush happens via the frame-end hook (registered in ui::init, run inside
  graphics::swap_frames after scene passes, before submit/Present).
  ui::end() is inert, kept for upstream call-site compatibility.

## M4b must handle (from final review, ordered by bite likelihood)

1. **reset_pt on resize** (main.cpp resize block ~880-908): resize during
   PT accumulation recreates display_tex uninitialized while pt_iteration
   stays nonzero → garbage accumulation until manual reset. One line
   (`reset_pt = true;` in the resize branch) — land it WITH the PT port.
2. **PT dispatch truncation** (main.cpp ~1386-1389, pre-existing):
   `screen_width / PT_GROUP_SIZE_X` integer division under-covers the last
   partial tile band — newly relevant with resizable windows. Round up
   (and mind the shader's own bounds behavior when doing so).
3. **HUD/histogram anchored to fixed SCREEN_X/SCREEN_Y** (main.cpp:1442,
   1485, 1537): overlay doesn't track live window size; vanishes if the
   window shrinks below initial height. Mechanical swap to live
   window_width/height, but it IS an upstream behavior change — give it
   its own adjudication line in the M4b plan.
4. **DPI-only resize miss** (main.cpp:877-879): resize detection keys on
   logical size; dragging between displays with different DPI changes only
   the framebuffer size → stale drawable scaled by Metal (blurry, no
   error). Also compare fb size; reconfigure surface only.
5. **get_panel_rect latent assert** (cpplib/ui.cpp:139-148): uses
   g_frame_open as a proxy for "inside start_panel/end_panel"; wrong proxy —
   GetWindowPos outside Begin/End hits an ImGui assert. Zero call sites
   today; fix if M4b adds one.
6. **No automated no-imgui-in-tests check**: only structural CMake fact +
   one-off otool run. A POST_BUILD nm/otool grep test is cheap insurance.
7. **No frame-lifecycle regression test**: consider a cheap
   ImGui::GetFrameCount()-based assert in ui::flush_frame() rather than a
   windowed test target.

## Inherited M3 items still open (see m3-carryovers.md)

- Missing per-draw `set_constant_buffer(&rendering_settings_buffer, 0)`
  before the three volume draw_mesh calls (vs_3d declares @group(0) —
  first volume draw hits draw_mesh's loud fatal without it).
- Blend alpha-channel formula is a guess — screenshot-compare volume slabs
  against upstream before trusting.
- 28-byte stride path has zero draw coverage — add a headless 28B draw
  test with/before the vs_3d port.
- Palette TGA loading (load_texture2D is a 1×1 white stub) + data/ CWD
  discipline.

## M5-critical (unchanged, see m2/m2b-carryovers.md)

- save_texture3D is a warn-once STUB while F6 export runs live.
- Real SDSS input catalog NOT in repo; reference cubes at
  ~/Development/js/skymap/data/raw/mcpm/.
