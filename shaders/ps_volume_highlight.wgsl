// ============================================================================
// ps_volume_highlight.wgsl
// Draft port of shaders/ps_volume_highlight.hlsl (Polyphorm, D3D11, 76 lines)
// to WGSL / Dawn. M4b Task 6.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed. See
// docs/superpowers/research/m4/m4b-volume-pt-design.md §2.3 for the
// translation plan this file implements (same shape as §2.2/ps_volume_trace
// plus a tex_deposit sample and a histogram_base-scaled smoothstep
// highlight band).
//
// What this pass does: for each fragment of the "super quad" stack
// (vs_3d.wgsl's interstage texcoord), samples the 3D trace texture and a 3D
// deposit ("trail") texture, applies a trim-box early-out, blends two
// remapped-exponential false colors (trace-based blue/purple, deposit-based
// orange), then — inside a density band derived from highlight_density and
// histogram_base — smoothsteps in a green highlight overlay.
//
// Draw sites this backs (main.cpp:1309-1317, VM_VOLUME_HIGHLIGHT):
//   graphics::set_vertex_shader(&vertex_shader);           // vs_3d.wgsl
//   graphics::set_texture(&trace_tex, 0);                  // tex_trace
//   graphics::set_texture_sampler(&tex_sampler_trace, 0);
//   graphics::set_pixel_shader(&ps_volume_highlight);       // this file
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
//   struct copied verbatim from shaders/cs_particles_transform.wgsl / M4b
//   Task 4's ps_volume_trace.wgsl (see translation-notes.md (M3) §1 for the
//   full byte-offset table and the matrix column-major cross-check).
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
    trim_density: f32,                     // +220  HLSL:24  (read in trace attenuation: t = sample_weight * (trace - trim_density))

    world_width: f32,                      // +224  HLSL:25  (unused here)
    world_height: f32,                     // +228  HLSL:26  (unused here)
    world_depth: f32,                      // +232  HLSL:27  (unused here)
    screen_width: f32,                     // +236  HLSL:28  (unused here)

    screen_height: f32,                    // +240  HLSL:29  (unused here)
    sample_weight: f32,                    // +244  HLSL:30
    optical_thickness: f32,                // +248  HLSL:31
    highlight_density: f32,                // +252  HLSL:32

    galaxy_weight: f32,                    // +256  HLSL:33
    histogram_base: f32,                   // +260  HLSL:34
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
//   matches main.cpp:1309-1316: slot 0 = tex_trace/sampler, slot 1 =
//   trail_tex_A|B (tex_deposit)/tex_sampler_deposit).
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

// HLSL:42-76
@fragment
fn main(input: PixelInput) -> @location(0) vec4<f32> {
    var fragment: vec4<f32>;

    // HLSL:45-49  Trim-box early-out — x/y/z bounds only, no density term
    // (unlike ps_volume_trace.hlsl/ps_volume_overdensity.hlsl).
    if (input.texcoord_out.x < cfg.trim_x_min || input.texcoord_out.x > cfg.trim_x_max ||
        input.texcoord_out.y < cfg.trim_y_min || input.texcoord_out.y > cfg.trim_y_max ||
        input.texcoord_out.z < cfg.trim_z_min || input.texcoord_out.z > cfg.trim_z_max) {
        // HLSL:48  fragment = 0.0.xxxx;
        fragment = vec4<f32>(0.0);
    } else {
        // HLSL:51  float trace = tex_trace.Sample(tex_trace_sampler, input.texcoord_out.xyz);
        //   IMPLICIT float4->float truncation in HLSL (Sample() returns
        //   float4, assigned to a float local) — made explicit .x in WGSL
        //   (NOTES V6 precedent).
        //
        // DEVIATION (compile-technical, not a math change — same shape as
        // ps_volume_trace.wgsl's HLSL:55 case, DESIGN §2.2/§2.3): this
        // Sample() call sits inside the else-branch, gated by a per-fragment
        // (non-uniform) trim condition — unlike ps_volume_trace.hlsl:45's
        // unconditional-before-any-branch sample, HLSL:51/53 here are both
        // INSIDE the trim else-block. WGSL's derivative-uniformity rule
        // forbids implicit-derivative textureSample() from non-uniform
        // control flow; textureSampleLevel(..., 0.0) (explicit LOD) lifts
        // the restriction and is numerically IDENTICAL since every texture
        // this codebase creates has mipLevelCount == 1 (graphics.cpp's
        // make_texture; see ps_volume_trace.wgsl's HLSL:55 comment for the
        // full argument). Reused verbatim here for both the trace and
        // deposit samples below.
        let trace = textureSampleLevel(tex_trace, tex_trace_sampler, input.texcoord_out.xyz, 0.0).x;
        // HLSL:52  float t = sample_weight * (trace - trim_density);
        let t = cfg.sample_weight * (trace - cfg.trim_density);
        // HLSL:53  float deposit = tex_deposit.Sample(tex_deposit_sampler, input.texcoord_out.xyz);
        //   Same implicit float4->float truncation as HLSL:51 -> explicit .x.
        let deposit = textureSampleLevel(tex_deposit, tex_deposit_sampler, input.texcoord_out.xyz, 0.0).x;
        // HLSL:54  float d = galaxy_weight * deposit;
        let d = cfg.galaxy_weight * deposit;

        // HLSL:56  // fragment.rgb = remap(t, 0.3) * float3(0.0, 2.1, 15.0) + remap(d, 0.2) * float3(1.0, 0.05, 0.0);
        // HLSL:57  fragment.rgb = remap(t, 0.3) * float3(0.6, 0.05, 0.9) + remap(d, 0.2) * float3(1.0, 0.6, 0.0);
        //
        // Local vars used in place of HLSL's field-by-field `fragment.rgb =`
        // / `fragment.a =` statements (WGSL house style avoids partial-vec4
        // swizzle assignment — see ps_volume_trace.wgsl's full-vec4-
        // constructor precedent); purely mechanical restatement, not a math
        // change. The highlight branch below needs to read back both
        // components after this initial write, so plain `var`s stand in for
        // the two fields until the final vec4<f32>() construction.
        var frag_rgb = remap(t, 0.3) * vec3<f32>(0.6, 0.05, 0.9) + remap(d, 0.2) * vec3<f32>(1.0, 0.6, 0.0);
        // HLSL:58  fragment.a = (0.2*t + 0.1*d) * optical_thickness;
        var frag_a = (0.2 * t + 0.1 * d) * cfg.optical_thickness;

        // HLSL:60-61
        let highlight_low = cfg.highlight_density / sqrt(cfg.histogram_base);
        let highlight_high = cfg.highlight_density * sqrt(cfg.histogram_base);
        // HLSL:62-63 (commented-out alternate; kept as comment for 1:1 file
        // correspondence)
        // let highlight_low = cfg.highlight_density / sqrt(pow(cfg.histogram_base, 0.4));
        // let highlight_high = cfg.highlight_density * sqrt(pow(cfg.histogram_base, 0.4));

        // HLSL:64-71
        if (trace > highlight_low && trace < highlight_high) {
            // HLSL:65-66  smoothstep is a WGSL builtin with identical
            // semantics to HLSL's smoothstep.
            let smooth_weight = smoothstep(highlight_low, highlight_low + 0.1 * (highlight_high - highlight_low), trace)
                * (1.0 - smoothstep(highlight_high - 0.1 * (highlight_high - highlight_low), highlight_high, trace));
            // HLSL:67-68
            frag_rgb = (1.0 - smooth_weight) * frag_rgb
                + smooth_weight * remap(trace, 10.0) * vec3<f32>(0.0, 1.0, 0.0);
            // HLSL:69-70
            frag_a = (1.0 - smooth_weight) * frag_a
                + smooth_weight * remap(trace, 10.0) * cfg.optical_thickness;
        }

        fragment = vec4<f32>(frag_rgb, frag_a);
    }

    // HLSL:74  fragment.rgb *= 2.0; // Compensate for the drawing 1 stack of meta-quads instead of 3
    // QUIRK(single_stack_2x_compensation): kept for VAC parity — upstream draws 1 stack instead of 3
    fragment = vec4<f32>(fragment.rgb * 2.0, fragment.a);
    return fragment;
}
