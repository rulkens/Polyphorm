# M4b → M5 carryovers

From M4b (volume rendering + path tracing port), range 15389d1..HEAD.
All 12 implementation tasks complete with per-task reviews. Final
whole-branch review verdict: Ready with fixes (this commit lands those
fixes). Human visual gate (Task 13): PASSED 2026-08-13 — user confirmed
("M4b visual gate passed") after running the GUI on the full 324,901-pt
VAC catalog at the native 712x1200x728 grid. M4b fully closed.

## M5 must handle

### Critical blockers

1. **`save_texture3D` still stubbed** (cpplib/graphics.cpp ~1410): F6 export
   runs with a warn-once placeholder. M5-blocking for full feature delivery.

2. **Blend alpha-channel formula unverified** (m3-carryovers item #3):
   SrcAlpha/OneMinusSrcAlpha applied to both RGB+alpha channels. Screenshot-compare
   reading finds this channel against upstream reference; spec notes M5 reads
   the actual render cube, not captured pixels — use that gate.

3. **Palette format pending gate verdict** (graphics.cpp load_texture2D):
   TGA palettes currently loaded as RGBA8_UNORM. Adjudicate vs RGBA8_SRGB
   (color-space interpretation matters for ps_volume shaders' remap operations).

### Deferred upstream behavior changes

4. **HUD/histogram anchored to fixed SCREEN_X/SCREEN_Y** (main.cpp:1442,
   1485, 1537): overlay geometry still baked to initial window size, vanishes
   if window shrinks below that height. Mechanical fix exists (swap to live
   `window_width/height`); deferred as upstream behavior change pending
   explicit re-adjudication.

5. **DPI-only resize miss** (main.cpp:877-879): resize detection keys on
   logical size; dragging between displays with different DPI changes only
   framebuffer size (Metal scales the old drawable → blurry, silent). Also
   compare `fb_width/height`; reconfigure surface only if framebuffer changed.

6. **`get_panel_rect` latent assert** (cpplib/ui.cpp:139-148): proxy for
   "inside ImGui panel" uses `g_frame_open`, but correct guard is
   `ImGui::IsItemVisible()` or explicit Begin/End state. Zero call sites
   today; fix if M5 adds one.

### Dead shaders (ported, not wired)

7. **`ps_volume_halocolor` / `ps_volume_velocity` ported but never bound**
   (main.cpp VM_VOLUME regime switch). Both shaders exist, compile, and inherit
   QUIRK(single_stack_2x_compensation) from M3 upstream. To activate: flip
   the `HALO_COLOR_ANALYSIS`/`VELOCITY_ANALYSIS` `#ifdef` logic (toggles around
   main.cpp:1716-1740, texture setup near main.cpp:532) and verify draw_mesh binds
   (`ported-not-wired`, out of scope). Same TGA-sampling coverage as ps_volume_trace
   (palette already live).

## Inherited open items (unchanged)

From m3-carryovers.md:
- Blend alpha formula (item #3) — see critical blockers above.

From m4a-carryovers.md:
- `ps_volume_halocolor`/`ps_volume_velocity` ported-not-wired (item #4) —
  see Dead shaders above.
- Inherited M3 items: per-draw constant-buffer binds (CLOSED M4b 54b5d67),
  28-byte stride coverage (CLOSED M4b 220ec90), palette TGA (CLOSED M4b
  e62a434).

## Deferred minors from M4b reviews

1. **ps_volume_highlight.wgsl line 53 trim_density comment**: Original wording
   claimed field "unused here"; corrected to note actual read site in trace
   attenuation (t = sample_weight * (trace - trim_density)).

2. **tests/render_path_tests.cpp line 912 unset_texture_sampled_compute call**:
   Added inline comment clarifying that this call also clears the paired
   sampler at binding 16+slot — load-bearing for strict-match contract,
   non-obvious at call site.

3. **HUD/histogram overlay geometry still anchored to fixed SCREEN_X/SCREEN_Y**
   (m4a-carryovers item #3): Deferred as upstream behavior change. Already
   documented above.

4. **Behavior divergence (benign, unavoidable): PARTICLES→PT round-trip**: Upstream's
   PT accumulator was display_tex, which VM_PARTICLES overwrites — so upstream,
   resuming PT at pt_iteration = N > 0 after visiting particles mode, folded the
   particle image into the running average (visible ghost). Our separate pt_accum_buffer
   is untouched by other modes, so PT resumes from pristine history and may look
   *cleaner* than upstream on mode round-trips. Not a palette/blend defect.

## Inherited open items (from m4a-carryovers.md)

1. **No automated no-imgui-in-tests check**: Only structural CMake fact +
   one-off otool run. A POST_BUILD nm/otool grep test is cheap insurance.

2. **No frame-lifecycle regression test**: Consider a cheap
   ImGui::GetFrameCount()-based assert in ui::flush_frame() rather than a
   windowed test target.

3. **M5 validation inputs**: Reference cubes live at
   `~/Development/js/skymap/data/raw/mcpm/` (incl. mcpm_sdss_d2/d4/d8.npy).
   The repo DOES ship the upstream SDSS *visualization* slice at
   `bin/data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0.bin`
   (37,655 galaxies) — usable for visual gates and M5 runs; verify at M5
   whether it is the same input that produced the published VAC cubes
   before trusting the Pearson comparison.

## Post-validation cleanup tickets (one per QUIRK)

M4b verified 7 QUIRK markers per the top-level spec. For each, a post-M4
cleanup ticket exists:

1. **QUIRK(single_stack_2x_compensation)** (ps_volume_*.wgsl ×5):
   Upstream draws 1 stack; we preserve 2×. Audit whether the difference is
   intentional (VAC fidelity) or a missed consolidation opportunity.

2. **QUIRK(overdensity_constant_remap)** (ps_volume_overdensity.wgsl:166):
   Brightness term uses literal e≈2.71; verify this hardcoded value was
   intentional and not a parameter bucket.

3. **QUIRK(seed_idx_truncation)** (cs_volpath.wgsl:535):
   uint3→uint assignment in RNG seed indexing. Verify no precision loss
   relative to upstream (index space is typically ≤1B entries).

4. **QUIRK(sky_scalar_truncation)** (cs_volpath.wgsl:345, 461, 632):
   get_sky_L declares float but returns float3 value; implicit scalar
   coercion on three call sites. Audit whether this is correct atmospheric
   scattering (single-channel radiance) or a ported bug.

5. **QUIRK(set_seed_mw_recheck)** (cs_volpath.wgsl:180):
   RNG.set_seed's second guard re-checks m_w==0 after first guard. Verify
   this is necessary (e.g., for seed=0x00000000 edge case) and not
   redundant defensive code.

6. **QUIRK(oob_dispatch_guard)** (cs_volpath.wgsl:525):
   NEW vs HLSL: explicit guard against ceil(screen/tile) tail invocations.
   Document why WGSL needs this where D3D11 typed-UAV OOB clamping sufficed.

7. **QUIRK(pt_ceil_dispatch)** (main.cpp:1409):
   Upstream uses integer division (screen/10); we use ceil. Verify this is
   correct (tile-boundary coverage) and not a ported change creep.

## M5 human visual gate (TBD by coordinator after Task 13)

[Outcome to be filled by plan coordinator after M4b final review.]
