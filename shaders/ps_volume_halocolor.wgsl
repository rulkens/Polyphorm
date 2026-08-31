// ============================================================================
// ps_volume_halocolor.wgsl
// Draft port of shaders/ps_volume_halocolor.hlsl (Polyphorm, D3D11, 75 lines)
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
// (vs_3d.wgsl's interstage texcoord), computes a 7-tap finite-difference
// gradient of the 3D trace texture (a crude Laplacian-like "edge" measure),
// applies a trim-box early-out, and blends a gray trace-edge tone with a
// blue/red deposit-sign tint driven by tex_deposit's two channels.
//
// Would back this draw site if wired (main.cpp:1679-1691,
// #ifdef HALO_COLOR_ANALYSIS, VM_VOLUME_HALOCOLOR — NOT enabled in this
// build; shown here only to document the intended binding shape):
//   graphics::set_vertex_shader(&vertex_shader);           // vs_3d.wgsl
//   graphics::set_texture(&trace_tex, 0);                  // tex_trace
//   graphics::set_texture_sampler(&tex_sampler_trace, 0);
//   graphics::set_pixel_shader(&ps_volume_halocolor);       // this file
//   graphics::set_texture(&trail_tex_A|B, 1);               // tex_deposit
//   graphics::set_texture_sampler(&tex_sampler_deposit, 1);
//   ... update/set_constant_buffer(&rendering_settings_buffer, 0) ...
//   graphics::draw_mesh(&super_quad_mesh);
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) render-stage textures.
// ---------------------------------------------------------------------------

// HLSL:12-35  cbuffer ConfigBuffer : register(b4)
//   Per the M2b/M3/M4b convention (carryover I3), b4-register cbuffers move
//   to @group(0) @binding(0) in this fork. Full 336-byte RenderingConfig
//   struct copied verbatim from ps_volume_highlight.wgsl / M4b Task 6 (see
//   translation-notes.md (M3) §1 for the full byte-offset table and the
//   matrix column-major cross-check).
struct RenderingConfig {
    projection: mat4x4<f32>,               // +0    HLSL:14
    view: mat4x4<f32>,                     // +64   HLSL:15
    model: mat4x4<f32>,                    // +128  HLSL:16

    texcoord_map: i32,                     // +192  HLSL:17  (unused here)
    trim_x_min: f32,                       // +196  HLSL:18
    trim_x_max: f32,                       // +200  HLSL:19
    trim_y_min: f32,                       // +204  HLSL:20

    trim_y_max: f32,                       // +208  HLSL:21
    trim_z_min: f32,                       // +212  HLSL:22
    trim_z_max: f32,                       // +216  HLSL:23
    trim_density: f32,                     // +220  HLSL:24

    world_width: f32,                      // +224  HLSL:25
    world_height: f32,                     // +228  HLSL:26
    world_depth: f32,                      // +232  HLSL:27
    screen_width: f32,                     // +236  HLSL:28  (unused here)

    screen_height: f32,                    // +240  HLSL:29  (unused here)
    sample_weight: f32,                    // +244  HLSL:30
    optical_thickness: f32,                // +248  HLSL:31
    highlight_density: f32,                // +252  HLSL:32  (unused here)

    galaxy_weight: f32,                    // +256  HLSL:33
    histogram_base: f32,                   // +260  HLSL:34  (unused here)
    overdensity_threshold_low: f32,        // +264  (not in this HLSL cbuffer)
    overdensity_threshold_high: f32,       // +268  (not in this HLSL cbuffer)

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

// HLSL:7-10  Texture3D tex_trace : register(t0); SamplerState tex_trace_sampler : register(s0);
//            Texture3D tex_deposit : register(t1); SamplerState tex_deposit_sampler : register(s1);
//   Render bind-group convention: binding == 2*slot for the texture,
//   2*slot+1 for its paired sampler, where slot is the argument to
//   graphics::set_texture/set_texture_sampler (translation-notes.md §6;
//   would match main.cpp:1679-1691's HALO_COLOR_ANALYSIS block if wired:
//   slot 0 = tex_trace/sampler, slot 1 = trail_tex_A|B (tex_deposit)/
//   tex_sampler_deposit — see §1 of the design doc's resource table).
@group(1) @binding(0) var tex_trace: texture_3d<f32>;
@group(1) @binding(1) var tex_trace_sampler: sampler;
@group(1) @binding(2) var tex_deposit: texture_3d<f32>;
@group(1) @binding(3) var tex_deposit_sampler: sampler;

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

// HLSL:42-75
@fragment
fn main(input: PixelInput) -> @location(0) vec4<f32> {
    var fragment: vec4<f32>;

    // HLSL:45-49  Trim-box early-out — x/y/z bounds only, no density term
    // (same shape as ps_volume_highlight.hlsl).
    if (input.texcoord_out.x < cfg.trim_x_min || input.texcoord_out.x > cfg.trim_x_max ||
        input.texcoord_out.y < cfg.trim_y_min || input.texcoord_out.y > cfg.trim_y_max ||
        input.texcoord_out.z < cfg.trim_z_min || input.texcoord_out.z > cfg.trim_z_max) {
        // HLSL:48  fragment = 0.0.xxxx;
        fragment = vec4<f32>(0.0);
    } else {
        // HLSL:51-57  7-tap finite-difference gradient of tex_trace, all
        // seven Sample() calls sit inside this else-branch (per-fragment,
        // non-uniform control flow via the trim-box branch above).
        //
        // DEVIATION (compile-technical, not a math change — same shape as
        // ps_volume_trace.wgsl's HLSL:55 / ps_volume_highlight.wgsl's
        // HLSL:51,53 cases, DESIGN §2.2/§2.3/§2.5): WGSL's derivative-
        // uniformity rule forbids implicit-derivative textureSample() from
        // non-uniform control flow; textureSampleLevel(..., 0.0) (explicit
        // LOD) lifts the restriction and is numerically IDENTICAL since
        // every texture this codebase creates has mipLevelCount == 1
        // (graphics.cpp's make_texture; see ps_volume_trace.wgsl's HLSL:55
        // comment for the full argument). Reused verbatim here for all
        // seven trace taps below and the one deposit sample further down.
        //
        // HLSL:51  float trace = 2.0 * tex_trace.Sample(tex_trace_sampler, input.texcoord_out.xyz);
        //   IMPLICIT float4->float truncation in HLSL (Sample() returns
        //   float4, assigned to a float local) — made explicit .x in WGSL
        //   (NOTES V6 precedent). Applies to all seven taps below.
        var trace = 2.0 * textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz, 0.0).x;
        // HLSL:52
        trace -= 0.166 * textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz + vec3<f32>( 1.0 / cfg.world_width, 0.0, 0.0), 0.0).x;
        // HLSL:53
        trace -= 0.166 * textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz + vec3<f32>(-1.0 / cfg.world_width, 0.0, 0.0), 0.0).x;
        // HLSL:54
        trace -= 0.166 * textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz + vec3<f32>(0.0,  1.0 / cfg.world_height, 0.0), 0.0).x;
        // HLSL:55
        trace -= 0.166 * textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz + vec3<f32>(0.0, -1.0 / cfg.world_height, 0.0), 0.0).x;
        // HLSL:56
        trace -= 0.166 * textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz + vec3<f32>(0.0, 0.0,  1.0 / cfg.world_depth), 0.0).x;
        // HLSL:57
        trace -= 0.166 * textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz + vec3<f32>(0.0, 0.0, -1.0 / cfg.world_depth), 0.0).x;

        // HLSL:58  float t = sample_weight * (trace - trim_density);
        let t = cfg.sample_weight * (trace - cfg.trim_density);
        // HLSL:59  float2 deposit = tex_deposit.Sample(tex_deposit_sampler, input.texcoord_out.xyz).xy;
        let deposit = textureSampleLevel(tex_deposit, tex_deposit_sampler, input.texcoord_out.xyz, 0.0).xy;
        // HLSL:60  float2 d = galaxy_weight * deposit;
        let d = cfg.galaxy_weight * deposit;

        // HLSL:62  fragment.rgb = remap(t, 0.6) * float3(0.5, 0.5, 0.5);
        //
        // Local var used in place of HLSL's field-by-field `fragment.rgb =`
        // statement (WGSL house style avoids partial-vec4 swizzle
        // assignment — ps_volume_trace.wgsl / ps_volume_highlight.wgsl
        // precedent); purely mechanical restatement, not a math change.
        var frag_rgb = remap(t, 0.6) * vec3<f32>(0.5, 0.5, 0.5);

        // HLSL:63-68  if/else on d.y sign — no third (== 0.0) branch is ever
        // taken; the commented-out "// else" clause at HLSL:65 and its body
        // at HLSL:67-68 are dead source, kept as comments for 1:1 file
        // correspondence (translation-notes.md T11 precedent).
        if (d.y > 0.0) {
            // HLSL:64
            frag_rgb += remap(d.x, 0.1) * vec3<f32>(0.0, 0.0, 0.8);
        } else {
            // HLSL:66  else// if (d.y < 0.0)
            frag_rgb += remap(d.x, 0.1) * vec3<f32>(0.7, 0.0, 0.0);
        }
        // HLSL:67-68 (dead, upstream-commented third branch — never reachable):
        // else
        //     fragment.rgb += remap(d.x, 0.1) * float3(0.0, 0.6, 0.0);

        // HLSL:70  fragment.a = (0.3*t + 0.15*d.x) * optical_thickness;
        let frag_a = (0.3 * t + 0.15 * d.x) * cfg.optical_thickness;

        fragment = vec4<f32>(frag_rgb, frag_a);
    }

    // HLSL:73  fragment.rgb *= 2.0; // Compensate for the drawing 1 stack of meta-quads instead of 3
    // QUIRK(single_stack_2x_compensation): kept for VAC parity — upstream draws 1 stack instead of 3
    fragment = vec4<f32>(fragment.rgb * 2.0, fragment.a);
    return fragment;
}
