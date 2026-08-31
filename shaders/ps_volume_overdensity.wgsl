// ============================================================================
// ps_volume_overdensity.wgsl
// Draft port of shaders/ps_volume_overdensity.hlsl (Polyphorm, D3D11, 69
// lines) to WGSL / Dawn. M4b Task 6.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed. See
// docs/superpowers/research/m4/m4b-volume-pt-design.md §2.4 for the
// translation plan this file implements (single texture, three-way
// density-bucket color select).
//
// What this pass does: for each fragment of the "super quad" stack
// (vs_3d.wgsl's interstage texcoord), samples the 3D trace texture,
// applies a trim-box + density early-out, then buckets the trace density
// into one of three flat overdensity colors (under/mid/over), scaled by a
// trace-INDEPENDENT brightness term and an alpha falloff.
//
// Draw sites this backs (main.cpp:1327-1329, VM_VOLUME_OVERDENSITY):
//   graphics::set_vertex_shader(&vertex_shader);           // vs_3d.wgsl
//   graphics::set_texture(&trace_tex, 0);                  // tex_trace
//   graphics::set_texture_sampler(&tex_sampler_trace, 0);
//   graphics::set_pixel_shader(&ps_volume_overdensity);     // this file
//   ... update/set_constant_buffer(&rendering_settings_buffer, 0) ...
//   graphics::draw_mesh(&super_quad_mesh);
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) render-stage textures.
// ---------------------------------------------------------------------------

// HLSL:10-35  cbuffer ConfigBuffer : register(b4)
//   Per the M2b/M3/M4b convention (carryover I3), b4-register cbuffers move
//   to @group(0) @binding(0) in this fork. Full 336-byte RenderingConfig
//   struct copied verbatim from shaders/cs_particles_transform.wgsl / M4b
//   Task 4's ps_volume_trace.wgsl (see translation-notes.md (M3) §1 for the
//   full byte-offset table and the matrix column-major cross-check). This
//   HLSL cbuffer is the first one to declare overdensity_threshold_low/high
//   (HLSL:33-34) — both already present at their canonical offsets in the
//   shared struct.
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
    trim_density: f32,                     // +220  HLSL:22

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
    overdensity_threshold_low: f32,        // +264  HLSL:33
    overdensity_threshold_high: f32,       // +268  HLSL:34

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
//   matches main.cpp:1327-1329: only slot 0 = tex_trace/sampler is bound for
//   this mode — no second texture, unlike VM_VOLUME/VM_VOLUME_HIGHLIGHT).
@group(1) @binding(0) var tex_trace: texture_3d<f32>;
@group(1) @binding(1) var tex_sampler_trace: sampler;

// HLSL:1-5  PixelInput — this shader's fragment input is vs_3d.wgsl's
// VertexOutput (interstage contract: @builtin(position) + @location(0)
// vec3<f32> texcoord_out — must match vs_3d.wgsl exactly).
struct PixelInput {
    @builtin(position) position_out: vec4<f32>,
    @location(0) texcoord_out: vec3<f32>,
};

// HLSL:37-39  #define COLOR_UNDERDENSE/MIDDENSE/OVERDENSE — WGSL has no
// preprocessor macros; ported as module-level const vec4s, values verbatim
// (DESIGN §2.4).
const COLOR_UNDERDENSE = vec4<f32>(0.0, 0.0, 25.0, 0.08);
const COLOR_MIDDENSE   = vec4<f32>(0.0, 2.0, 0.0, 0.06);
const COLOR_OVERDENSE  = vec4<f32>(10.0, 0.0, 0.0, 0.15);

// HLSL:41-44
fn remap(val: f32, slope: f32) -> f32 {
    return 1.0 - exp(-slope * val);
}

// HLSL:46-69
@fragment
fn main(input: PixelInput) -> @location(0) vec4<f32> {
    // HLSL:48  float4 fragment;
    // `var fragment = vec4<f32>(0.0);` init: WGSL requires a value at
    // declaration (no uninitialized-read-then-branch-assign like HLSL's
    // bare `float4 fragment;`); the branch chain below (trim/else, then
    // the if/else-if/else-if density bucket) is exhaustive over trace's
    // real-number range so this init is never observably read — purely a
    // WGSL-mandated initializer, not a behavior change.
    var fragment = vec4<f32>(0.0);

    // HLSL:49  float trace = tex_trace.Sample(tex_sampler_trace, input.texcoord_out.xyz);
    //   Called unconditionally before any branch (same shape as
    //   ps_volume_trace.hlsl:45) -> plain textureSample() is safe here
    //   (uniform control flow at the call site), no textureSampleLevel
    //   workaround needed, matching ps_volume_trace.wgsl's precedent for
    //   its own unconditional tex_trace sample.
    //   IMPLICIT float4->float truncation in HLSL (Sample() returns float4,
    //   assigned to a float local) — made explicit .x in WGSL (NOTES V6
    //   precedent).
    let trace = textureSample(tex_trace, tex_sampler_trace, input.texcoord_out.xyz).x;

    // HLSL:50-55  Trim-box + density early-out.
    if (input.texcoord_out.x < cfg.trim_x_min || input.texcoord_out.x > cfg.trim_x_max ||
        input.texcoord_out.y < cfg.trim_y_min || input.texcoord_out.y > cfg.trim_y_max ||
        input.texcoord_out.z < cfg.trim_z_min || input.texcoord_out.z > cfg.trim_z_max ||
        trace < cfg.trim_density) {
        // HLSL:54  fragment = 0.0.xxxx;
        fragment = vec4<f32>(0.0);
    } else {
        // HLSL:57-62  Three-way density bucket select.
        if (trace < cfg.overdensity_threshold_low) {
            fragment = COLOR_UNDERDENSE;
        } else if (trace >= cfg.overdensity_threshold_low && trace < cfg.overdensity_threshold_high) {
            fragment = COLOR_MIDDENSE;
        } else if (trace >= cfg.overdensity_threshold_high) {
            fragment = COLOR_OVERDENSE;
        }

        // HLSL:63  fragment.rgb *= remap(2.71, sample_weight);
        // QUIRK(overdensity_constant_remap): remap(2.71, sample_weight) — 2.71 is a literal (≈e), the brightness term depends only on sample_weight, NOT on trace. Faithful; do not "fix".
        fragment = vec4<f32>(fragment.rgb * remap(2.71, cfg.sample_weight), fragment.a);
        // HLSL:64  fragment.a *= optical_thickness * remap(trace, 1.0);
        fragment = vec4<f32>(fragment.rgb, fragment.a * cfg.optical_thickness * remap(trace, 1.0));
    }

    // HLSL:67  fragment.rgb *= 2.0; // Compensate for the drawing 1 stack of meta-quads instead of 3
    // QUIRK(single_stack_2x_compensation): kept for VAC parity — upstream draws 1 stack instead of 3
    fragment = vec4<f32>(fragment.rgb * 2.0, fragment.a);
    return fragment;
}
