# M3 → M4 carryovers

From the M3 final whole-branch review (READY, c9ad07d..981ede0). M3 closed
with VM_PARTICLES rendering live: atomic splat buffer → blit → textured quad
draw, headless pixels-exist test with exact hand-derived values, 7/7 suites
green. Human visual gate: PASSED 2026-08-12 — user confirmed chain/filament
formation on a 6-chain synthetic dataset (2000 points, 1M agents, 128 grid);
Esc/close clean. M3 fully closed.

## M4 must handle (ordered by likelihood of biting)

1. **I1 — sampler-slot leakage across shader changes (Important).**
   `unset_texture` clears `.view` only; samplers are never cleared. Safe
   while every draw uses the same slot set; breaks the first time M4
   switches between shader variants with different binding sets (volume →
   particles leaves a phantom sampler entry → CreateBindGroup validation
   failure). Fix: clear `.sampler` in `unset_texture` (verified safe —
   every draw site re-binds its samplers each frame). ALSO adjudicate
   **main.cpp:1280**: `graphics::set_texture_sampler(&tex_sampler_deposit, 2)`
   is a RENDER-stage call in the middle of VM_PATH_TRACING's COMPUTE bind
   block — almost certainly an upstream `set_texture_sampler_compute` typo;
   as-is it permanently poisons render slot 2 on the first volpath frame.
2. **Compute-side texture+sampler same-slot collision** (still present):
   `set_texture_sampler_compute` zeroes the slot, clobbering the sampled
   view (graphics.cpp ~509). cs_volpath pairs texture+sampler at slots 1,
   3, 4. Adopt the RenderSlot-pair or 2N/2N+1 template for `run_compute`
   BEFORE porting cs_volpath.
3. **Missing `set_constant_buffer(&rendering_settings_buffer, 0)` before
   the three volume draw_mesh calls** (main.cpp ~1244/1250/1256 — only
   `update_` today). vs_3d declares @group(0); first volume draw hits
   draw_mesh's loud fatal. Add per-draw binds in the M4 port.
4. **Blend alpha-channel formula is a guess** (SrcAlpha/OneMinusSrcAlpha on
   both channels). Unobservable in M3 (a=1.0 always). M4's volume slabs
   vary alpha per fragment: screenshot-compare against upstream before
   trusting.
5. **28-byte stride path has zero coverage** — the vertex-layout table
   entry exists but nothing draws super_quad_mesh. Add a headless 28B draw
   test (harness exists: get_render_target + readback in
   render_path_tests.cpp) before/with the vs_3d port.
6. **Real UI makes stub-shader branches reachable.** ui_stub's add_toggle
   never mutates vis_mode; ImGui will. Volume/PT branches with `= {}`
   shaders abort on draw_mesh's assert (asserts are LIVE in the app
   binary). Land the M4 shaders with/before the UI, or gate every branch
   on is_ready.
7. **ImGui needs a render-pass entry point** — g_encoder is graphics.cpp
   internal; draw_mesh opens/closes its own pass per call. Design where the
   ImGui WebGPU backend records (likely a new graphics:: hook or an
   ImGui-owned pass after the mode draw).
8. **Palette TGA loading** (load_texture2D is a 1×1 white stub) + relative
   `data/` path CWD discipline; the M2b histo-shader OOB note; blit
   numthreads(1,1,1) reshape only if profiling demands.

## Small fixes folded into M4 (triaged fix-in-M4)

- Fold `topology` into the pipeline cache key (safer than a comment;
  TRIANGLESTRIP currently unused).
- draw_mesh group-1 CONTRACT comment — write it WITH the sampler caveat
  once I1's fix defines the real contract.
- graphics.h:19 stale comment (`display_tex_uint` deleted; R32_UINT itself
  still live).

## Human visual gate checklist (M3 closes when a human confirms)

Run: `build/gen_test_dataset /tmp/polyviz && cd /tmp/polyviz && <repo>/build/polyphorm --dataset /tmp/polyviz/testdata`

1. **Camera**: orbit with mouse — structure rotates coherently, zoom sane,
   not mirrored (matrix-convention errors look like mirroring).
2. **Coverage**: image fills the window as the camera frames it — a
   one-quadrant image = the logical/framebuffer 4×-off failure.
3. **Palette**: teal/green filamentous agents + saturating pure-red dots
   (galaxy splats). Red speckle in ultra-dense cores is the preserved
   10000-threshold quirk, NOT a bug. All-white/gray = sampling wrong.
4. **Brightness**: plausible contrast vs upstream screenshots (sRGB path
   first exercised here).
5. **Softness**: blocky 2× nearest-neighbor upscale on Retina is the
   adjudicated Option A tradeoff — expected, don't flag. DO flag smearing
   or tearing.
6. **Dynamics**: three blobs converge into filaments over seconds; title
   pass-counter increments; F2 resets and structure re-forms; Esc exits
   with no process left.
7. **Edges**: no anomalous bright pixel pinned at a corner (OOB clamp
   symptom).
8. Blend-formula and palette correctness CANNOT be judged in this mode —
   they are M4 screenshot-compare items; don't extrapolate.
