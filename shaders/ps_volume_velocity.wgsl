// ============================================================================
// ps_volume_velocity.wgsl
// Draft port of shaders/ps_volume_velocity.hlsl (Polyphorm, D3D11, 58 lines)
// to WGSL / Dawn. M4b Task 7.
//
// FILE-PARITY PORT (M4b adjudication): dead in REGIME_SDSS — VELOCITY_ANALYSIS
// / HALO_COLOR_ANALYSIS are #ifdef'd out (main.cpp:38-40); not wired, no
// draw-path tests.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed. See
// docs/superpowers/research/m4/m4b-volume-pt-design.md §2.5 for the
// translation plan this file implements (§1's inventory entry: dead-in-
// default-build shader, ported for file-parity only per the adjudicated
// scope decision).
//
// What this pass does: for each fragment of the "super quad" stack
// (vs_3d.wgsl's interstage texcoord), samples all 4 channels of tex_trace
// (intended, per HLSL:45's .rgba read, for VELOCITY_ANALYSIS builds where
// trace_tex is widened from <half> to <half4> — main.cpp comment near the
// #define, "the extra 3 channels storing the equilibrium mean unsigned
// orientation of the agents"), applies a trim-box early-out, and colors
// surviving fragments by squared g/b/a "orientation" magnitude while alpha
// comes from the r channel's density-like value.
//
// Would back this draw site if wired (main.cpp:1701-1712,
// #ifdef VELOCITY_ANALYSIS, VM_VOLUME_VELOCITY — NOT enabled in this build;
// shown here only to document the intended binding shape):
//   graphics::set_vertex_shader(&vertex_shader);           // vs_3d.wgsl
//   graphics::set_texture(&trace_tex, 0);                  // tex_trace
//   graphics::set_texture_sampler(&tex_sampler_trace, 0);
//   graphics::set_pixel_shader(&ps_volume_velocity);        // this file
//   ... update/set_constant_buffer(&rendering_settings_buffer, 0) ...
//   graphics::draw_mesh(&super_quad_mesh);
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) render-stage texture.
// ---------------------------------------------------------------------------

// HLSL:10-35  cbuffer ConfigBuffer : register(b4)
//   Per the M2b/M3/M4b convention (carryover I3), b4-register cbuffers move
//   to @group(0) @binding(0) in this fork. Full 336-byte RenderingConfig
//   struct copied verbatim from ps_volume_overdensity.wgsl / ps_volume_
//   halocolor.wgsl (M4b Task 7) — this HLSL cbuffer already declares all 22
//   of the non-PT scalars (through overdensity_threshold_high), same subset
//   as ps_volume_overdensity.hlsl (see translation-notes.md (M3) §1 for the
//   full byte-offset table and the matrix column-major cross-check).
struct RenderingConfig {
    projection: mat4x4<f32>,               // +0    HLSL:12
    view: mat4x4<f32>,                     // +64   HLSL:13
    model: mat4x4<f32>,                    // +128  HLSL:14

    texcoord_map: i32,                     // +192  HLSL:15  (unused here)
    trim_x_min: f32,                       // +196  HLSL:16
    trim_x_max: f32,                       // +200  HLSL:17
    trim_y_min: f32,                       // +204  HLSL:18

    trim_y_max: f32,                       // +208  HLSL:19
    trim_z_min: f32,                       // +212  HLSL:20
    trim_z_max: f32,                       // +216  HLSL:21
    trim_density: f32,                     // +220  HLSL:22  (unused here — no density term in this shader's trim early-out, same shape as ps_volume_highlight.hlsl)

    world_width: f32,                      // +224  HLSL:23  (unused here)
    world_height: f32,                     // +228  HLSL:24  (unused here)
    world_depth: f32,                      // +232  HLSL:25  (unused here)
    screen_width: f32,                     // +236  HLSL:26  (unused here)

    screen_height: f32,                    // +240  HLSL:27  (unused here)
    sample_weight: f32,                    // +244  HLSL:28
    optical_thickness: f32,                // +248  HLSL:29
    highlight_density: f32,                // +252  HLSL:30  (unused here)

    galaxy_weight: f32,                    // +256  HLSL:31  (unused here)
    histogram_base: f32,                   // +260  HLSL:32  (unused here)
    overdensity_threshold_low: f32,        // +264  HLSL:33  (unused here)
    overdensity_threshold_high: f32,       // +268  HLSL:34  (unused here)

    // Members below +272 are NOT in the HLSL cbuffer (main.cpp's
    // RenderingConfig continues to pt_iteration / PT sigma terms / etc.).
    // Declared anyway so this struct is byte-identical to the full C++ type
    // and reusable verbatim by every other render/compute pass.
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
// sizeof == 336, matches main.cpp:313's static_assert exactly.
@group(0) @binding(0) var<uniform> cfg: RenderingConfig;

// HLSL:7-8  Texture3D tex_trace : register(t0); SamplerState tex_sampler_trace : register(s0);
//   Render bind-group convention: binding == 2*slot for the texture,
//   2*slot+1 for its paired sampler, where slot is the argument to
//   graphics::set_texture/set_texture_sampler (translation-notes.md §6;
//   would match main.cpp's VELOCITY_ANALYSIS block if wired: slot 0 =
//   tex_trace/sampler — see §1 of the design doc's resource table).
@group(1) @binding(0) var tex_trace: texture_3d<f32>;
@group(1) @binding(1) var tex_sampler_trace: sampler;

// HLSL:1-5  PixelInput — this shader's fragment input is vs_3d.wgsl's
// VertexOutput (interstage contract: @builtin(position) + @location(0)
// vec3<f32> texcoord_out — must match vs_3d.wgsl exactly).
struct PixelInput {
    @builtin(position) position_out: vec4<f32>,
    @location(0) texcoord_out: vec3<f32>,
};

// HLSL:37-40
fn remap(val: f32, slope: f32) -> f32 {
    return 1.0 - exp(-slope * val);
}

// HLSL:42-58
@fragment
fn main(input: PixelInput) -> @location(0) vec4<f32> {
    var fragment: vec4<f32>;

    // HLSL:45  float4 phase = tex_trace.Sample(tex_sampler_trace, input.texcoord_out.xyz).rgba;
    //   Called unconditionally before any branch (same shape as
    //   ps_volume_trace.hlsl:45 / DESIGN §2.2's precedent) -> plain
    //   textureSample() is fine here: WGSL's derivative-uniformity rule is
    //   trivially satisfied by an unconditional top-of-function call, no
    //   textureSampleLevel(...,0.0) DEVIATION needed (contrast the
    //   trim-else-gated samples in ps_volume_trace.wgsl/ps_volume_highlight
    //   .wgsl/ps_volume_halocolor.wgsl, which DO need it).
    //
    //   .rgba is a full 4-component read of texture_3d<f32> — compiles
    //   against this binding's declared type regardless of the actually-
    //   bound texture's real channel count; a single-channel R32Float
    //   texture sampled this way returns (v, 0, 0, 1) per WGSL's texture
    //   sampling rules (task brief), matching the HLSL Texture3D.Sample()
    //   contract this shader was written against for the intended
    //   VELOCITY_ANALYSIS-widened 4-channel trace texture.
    let phase = textureSample(tex_trace, tex_sampler_trace, input.texcoord_out.xyz).rgba;

    // HLSL:46-49  Trim-box early-out — x/y/z bounds only, no density term
    // (same shape as ps_volume_highlight.hlsl).
    if (input.texcoord_out.x < cfg.trim_x_min || input.texcoord_out.x > cfg.trim_x_max ||
        input.texcoord_out.y < cfg.trim_y_min || input.texcoord_out.y > cfg.trim_y_max ||
        input.texcoord_out.z < cfg.trim_z_min || input.texcoord_out.z > cfg.trim_z_max) {
        // HLSL:49  fragment = 0.0.xxxx;
        fragment = vec4<f32>(0.0);
    } else {
        // HLSL:52  fragment.rgb = sample_weight * phase.gba * phase.gba * float3(0.6, 0.5, 1.0);
        // HLSL:53  fragment.a = remap(0.1 * phase.r, optical_thickness);
        //
        // Local vars used in place of HLSL's field-by-field `fragment.rgb =`
        // / `fragment.a =` statements (WGSL house style avoids partial-vec4
        // swizzle assignment — ps_volume_trace.wgsl / ps_volume_highlight
        // .wgsl precedent); purely mechanical restatement, not a math
        // change.
        let frag_rgb = cfg.sample_weight * phase.gba * phase.gba * vec3<f32>(0.6, 0.5, 1.0);
        let frag_a = remap(0.1 * phase.r, cfg.optical_thickness);
        fragment = vec4<f32>(frag_rgb, frag_a);
    }

    // HLSL:56  fragment.rgb *= 2.0; // Compensate for the drawing 1 stack of meta-quads instead of 3
    // QUIRK(single_stack_2x_compensation): kept for VAC parity — upstream draws 1 stack instead of 3
    fragment = vec4<f32>(fragment.rgb * 2.0, fragment.a);
    return fragment;
}
