// ============================================================================
// cs_particles_blit.wgsl
// Draft port of shaders/cs_particles_blit.hlsl (Polyphorm, D3D11) to
// WGSL / Dawn.
//
// STATUS: DRAFT — not compiled. See translation-notes.md (M3, this directory).
//
// What this pass does: reads the per-pixel splat accumulation produced by
// cs_particles_transform and writes a weighted float into display_tex, the
// texture ps_particles_color later samples. Runs [numthreads(1,1,1)] — one
// invocation per output pixel, dispatched (window_width, window_height, 1)
// by the original main.cpp.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed.
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) @binding(N) == HLSL
// register index N. Same RenderingConfig struct as cs_particles_transform.wgsl
// (see that file for the full field-by-field offset derivation); only
// `sample_weight` (+244) is actually read here.
// ---------------------------------------------------------------------------

struct RenderingConfig {
    projection: mat4x4<f32>,               // +0
    view: mat4x4<f32>,                     // +64
    model: mat4x4<f32>,                    // +128

    texcoord_map: i32,                     // +192
    trim_x_min: f32,                       // +196
    trim_x_max: f32,                       // +200
    trim_y_min: f32,                       // +204

    trim_y_max: f32,                       // +208
    trim_z_min: f32,                       // +212
    trim_z_max: f32,                       // +216
    trim_density: f32,                     // +220

    world_width: f32,                      // +224
    world_height: f32,                     // +228
    world_depth: f32,                      // +232
    screen_width: f32,                     // +236  USED (buffer row stride / bounds)

    screen_height: f32,                    // +240  USED (bounds)
    sample_weight: f32,                    // +244  USED — HLSL:36
    optical_thickness: f32,                // +248
    highlight_density: f32,                // +252

    galaxy_weight: f32,                    // +256
    histogram_base: f32,                   // +260
    overdensity_threshold_low: f32,        // +264
    overdensity_threshold_high: f32,       // +268

    camera_x: f32,                         // +272
    camera_y: f32,                         // +276
    camera_z: f32,                         // +280
    pt_iteration: i32,                     // +284

    sigma_s: f32,                          // +288
    sigma_a: f32,                          // +292
    sigma_e: f32,                          // +296
    trace_max: f32,                        // +300

    camera_offset_x: f32,                  // +304
    camera_offset_y: f32,                  // +308
    exposure: f32,                         // +312
    n_bounces: i32,                        // +316

    ambient_trace: f32,                    // +320
    compressive_accumulation: i32,         // +324
    guiding_strength: f32,                 // +328
    scattering_anisotropy: f32,            // +332
};
@group(0) @binding(0) var<uniform> cfg: RenderingConfig;

// HLSL:1  RWTexture2D<uint> tex_in: register(u0);
//   THE SAME atomic-storage-texture problem as cs_particles_transform.wgsl
//   (see that file's header) forces this to be the SAME underlying
//   wgpu::Buffer, reinterpreted here as PLAIN (non-atomic) u32.
//
//   This is safe and does not need atomic ops on the read side: the
//   transform pass and this blit pass are two SEPARATE compute dispatches
//   (main.cpp's original code issues them back-to-back on one command list:
//   `run_compute(10,10,grid_z)` for the transform, then unset/rebind, then
//   `run_compute(window_width, window_height, 1)` for the blit). WebGPU
//   inserts an implicit memory barrier between dispatches (same guarantee
//   D3D11's UAV hazard tracking gives between two back-to-back Dispatch
//   calls touching the same UAV), so every write from the transform pass is
//   visible to every read in this pass without needing atomic semantics
//   here — only the WRITE side (many-to-one, single dispatch) needed
//   atomicity. WGSL permits binding the identical buffer with a
//   non-atomic element type in a different shader module/pipeline; atomic<T>
//   and T have identical size/layout, so this is purely a shader-side type
//   annotation, not a resource-level property.
//
//   Row-major index, matching cs_particles_transform.wgsl's write side
//   exactly: idx = y * width + x, width = u32(cfg.screen_width).
@group(1) @binding(0) var<storage, read> tex_in: array<u32>;

// HLSL:2  RWTexture2D<float> tex_out: register(u1);
//
//   *** UNCERTAIN / FLAGGED — see translation-notes.md §7 for full writeup ***
//   The HLSL declares tex_out as a SINGLE-CHANNEL float UAV, but main.cpp
//   creates the bound resource (display_tex) as 4-channel
//   DXGI_FORMAT_R32G32B32A32_FLOAT (main.cpp, pre-M2b:577; current fork:
//   Format::RGBA32_FLOAT). A single-channel typed UAV view over a
//   concrete (non-typeless) 4-channel resource is not a same-bit-width
//   format-cast D3D11 normally allows, so this looks like a real upstream
//   type/resource mismatch, not an intentional narrowing. It is only
//   OBSERVABLE, and therefore only resolvable from the HLSL alone, via
//   ps_particles_color.hlsl:12 `float v = tex_trace.Sample(...)` — an
//   implicit truncation that reads the FIRST (red) component of the
//   sampled 4-vector. That confirms the intent: this shader's `val` lands
//   in the red channel. What D3D11 actually does to the G/B/A channels of a
//   mismatched-type write is driver-defined and NOT recoverable from static
//   reading. This draft writes (0.0, 0.0, 1.0) for G/B/A — a reasonable
//   default that makes "only red is meaningful" explicit — but that choice
//   is UNVERIFIED against real D3D11 output. It is currently inert (nothing
//   reads G/B/A today), so it is low-priority unless a future pass samples
//   more than .x from display_tex.
@group(1) @binding(1) var tex_out: texture_storage_2d<rgba32float, write>;

// ---------------------------------------------------------------------------
// Entry point
//
// HLSL:31  [numthreads(1, 1, 1)] — one invocation per workgroup, dispatched
// (window_width, window_height, 1) by main.cpp's original code. Far under
// any per-dimension or per-workgroup invocation limit (WG size is 1), so
// unlike the transform/agent/decay shaders there is no numthreads blocker
// here at all.
//
// PERFORMANCE NOTE, not a fidelity concern: dispatching one workgroup per
// pixel (e.g. ~2M single-thread workgroups at 1920x1080, ~8M at a 2x-Retina
// 3840x2160 framebuffer — see translation-notes.md's display-sizing
// recommendation) is likely to be measurably slower on Dawn/Metal than a
// reshaped e.g. @workgroup_size(8,8,1) with dispatch
// (ceil(width/8), ceil(height/8), 1) and an in-shader bounds guard. This is
// NOT a preserved quirk (there is no observable behavioural difference
// between the two dispatch shapes — every invocation still touches exactly
// one texel, independently, with no shared state), so reshaping it would be
// a pure perf win with zero fidelity risk, unlike the WG(10,10,10) reshape
// discussion in the M2 notes. Left as literal (1,1,1) in this draft;
// flagged as an easy M3 follow-up, not a required change.
// ---------------------------------------------------------------------------

@compute @workgroup_size(1, 1, 1)
fn main(
    // HLSL:32-33 also take SV_GroupThreadID and SV_GroupID as params but
    // never read them in the body — dropped, matching the cs_field_decay
    // precedent (M2 notes D14). WGSL's global_invocation_id is the standard
    // equivalent of SV_DispatchThreadID.
    @builtin(global_invocation_id) dispatch_id: vec3<u32>,
) {
    let coord = dispatch_id.xy;                                  // HLSL:33 dispatchThreadId.xy

    // HLSL:34  float val = tex_in[dispatchThreadId.xy];
    //   Texture-coordinate read reinterpreted as a row-major buffer index —
    //   see the binding comment above. width MUST equal the actual dispatch
    //   width (== the buffer's row stride) for this to line up; that
    //   invariant is a host-side (main.cpp) responsibility, not something
    //   the shader can assert.
    let width_px = u32(cfg.screen_width);
    let idx = coord.y * width_px + coord.x;
    let val = f32(tex_in[idx]);

    // HLSL:35-39
    //   QUIRK(hardcoded_10000_threshold): the literal `10000` here is not
    //   sourced from the uniform; it is the exact weight the transform pass
    //   uses for a galaxy/data-point splat (HLSL cs_particles_transform.hlsl:99).
    //   A single agent-weight (10) splat can never reach 10000 on its own,
    //   but MANY agents landing on the same pixel legitimately can (1000+
    //   overlapping agents), and would then be misclassified as "a galaxy
    //   pixel" and skip the sample_weight scaling below. This is a genuine
    //   upstream ambiguity between "this pixel has a galaxy" and "this pixel
    //   has a lot of agents" baked into the choice of magic-number
    //   thresholds in the two shaders; kept verbatim, not fixed.
    var val_out: f32;
    if (val < 10000.0) {
        val_out = val * cfg.sample_weight;                        // HLSL:36
    } else {
        val_out = val;                                            // HLSL:38
    }

    // HLSL:36,38  tex_out[dispatchThreadId.xy] = ...;
    //   See the *** UNCERTAIN *** note on the tex_out binding above re: the
    //   G/B/A channel values.
    textureStore(tex_out, vec2<i32>(coord), vec4<f32>(val_out, 0.0, 0.0, 1.0));
}
