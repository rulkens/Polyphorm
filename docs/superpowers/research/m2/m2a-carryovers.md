# M2a → M2b carryovers

Obligations and traps from the M2a final whole-branch review that the M2b
plan MUST pick up. M2a closed at `dafa63f` (graphics:: layer over WebGPU:
lifecycle, resources, compute, sync readback, render stubs; 3 headless GPU
test suites green, asserts verified active in tests AND the app binary).

## Mandatory M2b-plan requirements (from final review)

- **I3 — three shaders use `cbuffer ConfigBuffer : register(b4)`**, bound
  persistently once at startup (`main.cpp:788`, `rendering_settings_buffer`):
  `cs_volpath.hlsl`, `cs_particles_transform.hlsl`, `cs_particles_blit.hlsl`.
  The fork's uniform model is single per-dispatch group-0 binding at slot 0.
  M2b must (a) move those WGSL cbuffers to `@group(0) @binding(0)` and
  (b) call `set_constant_buffer(&rendering_settings_buffer, 0)` before EACH
  of those dispatches. Forgetting one yields matching-layout wrong-data with
  NO validation error (SimulationConfig read as RenderingConfig).
- **I4 — full bind-slot discipline at ALL 7 dispatch sites, both groups.**
  Original main.cpp unsets textures but never structured buffers (D3D11
  leave-it-bound habit). Under the exact-match group-1 contract, stale buffer
  slots → Dawn bind-group-layout mismatch → pass invalidated (stderr-loud),
  or in the layout-coincidence case, silent wrong data. Each dispatch site
  must set exactly what its shader declares and unset the rest. Buffer slots
  can be cleared via `unset_texture_compute` (all unsets do the same `{}`
  reset) or M2b may add an `unset_structured_buffer` alias to graphics.h
  (the M2a freeze on the header ends with M2a).
- **run_compute group-0 heuristic** (Task 5 Important): binds group 0 iff
  `g_uniform_buffer` non-null (shadow-state presence), not from the shader's
  declared layout. M2b plan must either harden run_compute (derive group-0
  need from the pipeline/shader) or make the per-dispatch
  `set_constant_buffer` discipline above explicit at every site. Reproduced
  failure: shader without group-0 uniform + stale shadow state → Dawn
  "binding index 0 not present in layout []" → dispatch silently dropped.
- **The 256-iteration agent-sort loop** (`main.cpp:1019-1025`) interleaves
  `update_constant_buffer` with recorded dispatches. This now works
  correctly (C1 fix: update_* flushes the encoder first) but each update
  costs a queue submit — 256 submits/frame in that loop. Correct first;
  perf-optimize later if the sort loop is hot (it is the first place to look).
- **Deferred minors ruled fix-in-M2b** by the final review:
  - file_system test: assert `((char*)f.data)[f.size] == 0` (NUL contract
    that `get_compute_shader_from_code` load-bears on).
  - `clear_texture(Texture3D*)`: add format assert (r32float) matching the
    2D variants.

## M2b watch list (correct in M2a, easy to silently misuse)

1. Uniform binding is per-dispatch shadow state, not a persistent hardware
   slot (see I3).
2. Group-1 exact-match contract vs. D3D11 leave-it-bound habits (see I4).
   If binding semantics are ever tightened to auto-clear after dispatch, the
   sort loop must move its binds inside the loop.
3. Mid-frame `update_constant_buffer` is safe (C1 flush) but each call is a
   queue submit.
4. **R16→R32 conversion**: `get_texture2D/3D` accept and IGNORE
   `pixel_byte_count`, recomputing upload size from the fork Format. Passing
   half-float (2-byte) texel data with `R32_FLOAT` under-uploads without
   error. main.cpp's half source data must be converted to f32 before
   upload. Memory doubles: ~2.5 GB per VAC-scale 3D texture, three of them
   (~7.5 GB total — fits in 64 GB unified, mind other apps).
5. **Framebuffer vs logical blit mismatch**: `ctx.width/height` (and
   `get_render_target_window`) are FRAMEBUFFER pixels (Retina 2x);
   main.cpp sizes `display_tex` and the blit dispatch at logical
   `window_width/height` (`main.cpp:1183`) — 4x coverage mismatch on Retina
   unless one convention is chosen at port time. Decide in the M2b plan.
6. Nonzero `clear_texture` values are safe now (I1 flush) but were the
   latent trigger for the shared-scratch-uniform bug — first nonzero-clear
   call site is worth a glance in review.
7. `capture_structured_buffer` busy-blocks (intended D3D11 Map parity);
   capture_agents mode = 4 full pipeline stalls per frame.
8. Don't `release()` a ComputeShader still selected via
   `set_compute_shader`; asserts catch it now that they're armed in the app
   binary too (`-UNDEBUG` on the polyphorm target).

## State of the layer at M2a close

- Tests: `ctest --test-dir build` → cpplib_tests, file_system_tests,
  graphics_tests (5 GPU tests incl. C1 write-ordering regression and 2D
  uint clear roundtrip). All headless via `gpu::init_device`.
- Granted limits (M1 Max): maxComputeInvocationsPerWorkgroup=1024 (original
  `numthreads(10,10,10)`/`(8,8,8)` fit unchanged — no reshape),
  maxStorageBufferBindingSize=4095 MiB, maxBufferSize=4095 MiB,
  maxTextureDimension3D floor-gated ≥1024.
- Binding convention: `@group(0) @binding(0)` uniform config;
  `@group(1) @binding(N)` slot-N resource (N = HLSL register index).
- `g_clear2d_f` (RGBA32_FLOAT 2D clear) shares the tested code path but has
  no dedicated end-to-end test (disclosed gap, accepted).
- Untriaged leftovers accepted as-is by the final review: GetLimits status
  discarded; init bool-returns unreachable-false; SRGB StorageBinding
  unreachable; capture stride%4 alignment unexercised; get_mesh unaligned
  WriteBuffer (M3 adds assert if odd strides appear); graphics_tests
  recompiles shared sources (consider a cpplib object library when M2b adds
  files).
