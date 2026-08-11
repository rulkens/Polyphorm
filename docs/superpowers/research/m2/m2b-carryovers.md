# M2b → M3/M4/M5 carryovers

Obligations and traps from the M2b final whole-branch review that the M3
(particle rendering), M4 (volume + ImGui), and M5 (export + VAC validation)
plans must pick up. M2b closed with the simulation running: headless energy
smoke E 77.6 → 345.6 over 400 iterations (~0.5 s wall on the M1 Max), 6/6
ctest suites green.

## Verified sound (do not re-litigate)

- **is_a ping-pong orientation is byte-identical to upstream D3D11** —
  propagate flips `is_a` then binds A-as-deposit when `is_a`; decay reads
  slot 0 = A / writes slot 1 = B when `is_a`. The translation-notes
  off-by-one-frame concern does NOT apply.
- OOB-zero emulation (QUIRK(oob_load_zero_emulation)) covers every
  sim-state load; the one unguarded propagate load is provably dead (feeds
  only a same-coordinate discarded store). OOB store discard is
  test-pinned.
- Reproducibility is STATISTICAL by design: the racy non-atomic deposit
  RMW (preserved quirk) gives ±5% run-to-run E variance under GPU
  scheduling. **M5 acceptance thresholds must be statistical, never
  bit-exact.** The f16→f32 deposit widening (translation-notes §7.10) is a
  known, expected correlation-loss source to cite in the M5 report.

## M3 (particle rendering) must handle

1. **display_tex sizing decision**: `display_tex`/`display_tex_uint` are
   logical `window_width/height` (marker comment at creation site);
   `get_render_target_window` and ctx dims are FRAMEBUFFER pixels (Retina
   2×). Pick one convention before porting `cs_particles_blit` or coverage
   is 4× off.
2. **VM_PARTICLES restoration**: the deleted `ClearUnorderedAccessViewUint`
   returns as `graphics::clear_texture_uint(&display_tex_uint, 0)`;
   `display_tex_uint` is still allocated/released and ready.
3. **Stub vs/ps shaders lie**: `get_vertex_shader_from_code` /
   `get_pixel_shader_from_code` return `valid=true` WITHOUT compiling.
   The moment M3 loads real shader sources these must become real
   compiles (WGSL vertex/fragment + error scopes, like the compute path)
   or `is_ready` gives false confidence.
4. **Blend state**: `set_blend_state(ALPHA)` records shadow state only;
   M3's render pipelines must actually consume `g_blend`.
5. **If cs_agents_sort is ported** (upstream default-off, toggle commented
   out): the sort block now unsets buffers 2-7 (final-review fix), but
   note the 256-queue-submit-per-frame cost of its
   update_constant_buffer-in-loop pattern (C1 flush semantics).
6. `using namespace graphics;` in main.cpp — verified collision-free
   today; check for ambiguation whenever graphics:: grows.
7. Window resizability still forced off; if M3 renders interactively,
   surface-reconfigure-on-resize belongs in graphics::.
8. `load_texture2D` returns a 1×1 white stub — palettes must load for
   real (TGA) in M3/M4; windowed runs need CWD discipline for relative
   `data/` paths (shaders use absolute SHADER_ROOT; data does not).

## M4 (volume + ImGui) must handle

- **Volpath per-dispatch uniform**: the volpath site now binds
  `rendering_settings_buffer` at slot 0 per-dispatch (final-review fix) —
  when M4 ports cs_volpath.wgsl, its cbuffer moves from register(b4) to
  `@group(0) @binding(0)` (carryover I3; same for particles_transform /
  particles_blit in M3).
- RenderingConfig is 336 B (3 mat4 + 36 scalars, static_assert'd) — the
  WGSL struct must match offsets exactly; matrices are column-major both
  sides but verify against how Matrix4x4 is filled.
- ImGui pins: v1.92.9 (01380c57) + ImPlot v1.0 (524f9fcd), install order
  platform-callbacks-first (docs/superpowers/research/m2/imgui-integration.md).
- Histo-shader trace load (cs_density_histo.wgsl:106) is the one sim-shader
  load without an OOB-zero guard — stats-only impact; add the guard or a
  comment when next touching the file (final-review ruling: fix-in-M3+).

## M5 (export + validation) must handle — CRITICAL, easy to forget

- **`graphics::save_texture3D` is a warn-once STUB** while main.cpp's F6
  export block runs live (writes metadata + halos CSV but NO volume data).
  M5's Pearson comparison IS this export. It must be a first-class task in
  the M4 or M5 plan: texture3D → staging buffer → mapAsync → file, at
  VAC scale (~2.5 GB per texture). `save_texture2D_HDR` and
  `capture_current_frame` are stubs too (screenshots).
- VAC-scale sanity already verified: grid 712×1200×728 within limits,
  particle buffers ≈41 MB, u32 seed products safe, dispatch_truncation
  drops the same tail agents upstream dropped.
- Real SDSS input catalog (`data/SDSS/sdssGalaxy_rsdCorr_dbscan_...`) is
  NOT in the repo (upstream distributed it separately) — obtain before M5
  fitting. Published reference cubes are local at
  ~/Development/js/skymap/data/raw/mcpm/ (d2/d4/d8 npy).

## Accepted-as-is (final-review triage, do not re-flag)

- `--headless N` runs N+1 iterations (documented off-by-one in the plan's
  own snippet).
- `logging::print(char*)` signature + 3 warnings; `statistics_config_buffer`
  never released at exit (upstream omission); guarded-load branch cost
  unprofiled (profile at M5 only if tight); uses_group0 `= false`
  initializer; unset_structured_buffer placement/brace style.
