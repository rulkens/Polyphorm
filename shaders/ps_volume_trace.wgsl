// ============================================================================
// ps_volume_trace.wgsl
// Draft port of shaders/ps_volume_trace.hlsl (Polyphorm, D3D11, 95 lines) to
// WGSL / Dawn. M4b Task 4.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed. See
// docs/superpowers/research/m4/m4b-volume-pt-design.md §2.2 for the
// translation plan this file implements.
//
// What this pass does: for each fragment of the "super quad" stack
// (vs_3d.wgsl's interstage texcoord), samples the 3D trace texture, applies
// a trim-box early-out + a density threshold, and — for surviving fragments
// — remaps the trimmed density through an exponential curve and looks that
// remapped value up in a 1D (2D-image, sampled at v=0.5) false-color
// palette to produce the final teal-filament color.
//
// Draw sites this backs (main.cpp:1298-1306, VM_VOLUME):
//   graphics::set_vertex_shader(&vertex_shader);           // vs_3d.wgsl
//   graphics::set_texture(&trace_tex, 0);                  // tex_trace
//   graphics::set_texture_sampler(&tex_sampler_trace, 0);
//   graphics::set_pixel_shader(&pixel_shader);              // this file
//   graphics::set_texture(&palette_trace_tex, 1);           // tex_false_color
//   graphics::set_texture_sampler(&tex_sampler_color_palette, 1);
//   ... update/set_constant_buffer(&rendering_settings_buffer, 0) ...
//   graphics::draw_mesh(&super_quad_mesh);
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) render-stage textures.
// ---------------------------------------------------------------------------

// HLSL:12-35  cbuffer ConfigBuffer : register(b4)
//   Per the M2b/M3/M4b convention (carryover I3), b4-register cbuffers move
//   to @group(0) @binding(0) in this fork. The HLSL here declares 29 of
//   RenderingConfig's 36 scalars (plus all 3 matrices); the full 336-byte
//   struct is declared below anyway so ONE uniform buffer + ONE @group(0)
//   bind group can be shared with every other render/compute pass. Struct
//   text copied verbatim from vs_3d.wgsl / cs_particles_transform.wgsl (M3)
//   — see translation-notes.md (M3) §1 for the full byte-offset table and
//   the matrix column-major cross-check (no transpose needed anywhere).
//
//   NAMING NOTE (DESIGN §2 intro, not a bug): HLSL:25-27 name the
//   world_width/height/depth-offset members `world_X/world_Y/world_Z`. This
//   is upstream naming drift at the same struct offset (main.cpp:729-731
//   sets these fields from GRID_RESOLUTION_X/Y/Z — a voxel count, not a
//   physical size) — cs_volpath.hlsl's `grid_x/grid_y/grid_z` name is the
//   more accurate one. This port uses the canonical RenderingConfig field
//   names (world_width/world_height/world_depth), matching every other WGSL
//   port of this shared struct. world_X/Y/Z do not appear anywhere in this
//   shader's body (HLSL:42-95) — they're declared in the cbuffer but never
//   read by ps_volume_trace.hlsl's main() — so this naming choice is inert
//   here regardless.
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

    world_width: f32,                      // +224  HLSL:25  (world_X — see naming note above; unused in body)
    world_height: f32,                     // +228  HLSL:26  (world_Y — unused in body)
    world_depth: f32,                      // +232  HLSL:27  (world_Z — unused in body)
    screen_width: f32,                     // +236  HLSL:28  (unused here)

    screen_height: f32,                    // +240  HLSL:29  (unused here)
    sample_weight: f32,                    // +244  HLSL:30
    optical_thickness: f32,                // +248  HLSL:31
    highlight_density: f32,                // +252  HLSL:32  (unused here)

    galaxy_weight: f32,                    // +256  HLSL:33  (unused here — only read by the commented-out Proxy draws config block below)
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

// HLSL:7-10  Texture3D tex_trace : register(t0); SamplerState tex_sampler_trace : register(s0);
//            Texture2D tex_false_color : register(t1); SamplerState tex_false_color_sampler : register(s1);
//   Render bind-group convention: binding == 2*slot for the texture,
//   2*slot+1 for its paired sampler, where slot is the argument to
//   graphics::set_texture/set_texture_sampler (translation-notes.md §6;
//   confirmed load-bearing at cpplib/graphics.cpp:837,842 and matching
//   main.cpp:1299-1306 exactly: slot 0 = tex_trace/sampler, slot 1 =
//   tex_false_color/sampler).
@group(1) @binding(0) var tex_trace: texture_3d<f32>;
@group(1) @binding(1) var tex_sampler_trace: sampler;
@group(1) @binding(2) var tex_false_color: texture_2d<f32>;
@group(1) @binding(3) var tex_false_color_sampler: sampler;

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

// HLSL:42-95
@fragment
fn main(input: PixelInput) -> @location(0) vec4<f32> {
    var fragment: vec4<f32>;

    // HLSL:45  Texture3D.Sample(), called unconditionally before any branch
    //   -> textureSample() on texture_3d<f32>. textureSample is a
    //   fragment-shader-only construct requiring non-uniform-control-flow
    //   derivatives (WGSL's derivative-uniformity rule) — calling it here,
    //   unconditionally at the top of main() before any branch, matches the
    //   HLSL's own unconditional-sample-before-branch shape and keeps this
    //   trivially satisfied (DESIGN §2.2). Keep the explicit .r (Texture3D
    //   is declared single-channel-consumed here, same implicit-truncation
    //   shape as M3's ps_particles_color.hlsl:12 / V6).
    let trace = textureSample(tex_trace, tex_sampler_trace, input.texcoord_out.xyz).r;

    // HLSL:46-52  Trim-box early-out (ported verbatim, including the
    // commented-out "// false) {" — see HLSL:49, a dead debug toggle left
    // in place upstream; kept as a comment for 1:1 file correspondence).
    if (input.texcoord_out.x < cfg.trim_x_min || input.texcoord_out.x > cfg.trim_x_max ||
        input.texcoord_out.y < cfg.trim_y_min || input.texcoord_out.y > cfg.trim_y_max ||
        input.texcoord_out.z < cfg.trim_z_min || input.texcoord_out.z > cfg.trim_z_max ||
        // false) {
        trace < cfg.trim_density) {
        fragment = vec4<f32>(0.0, 0.0, 0.0, 0.0);
    } else {
        // HLSL:54  float t = (trace - trim_density) * sample_weight;
        let t = (trace - cfg.trim_density) * cfg.sample_weight;
        // HLSL:55  fragment.rgb = tex_false_color.Sample(tex_false_color_sampler, float2(remap(t, 1.0), 0.5)).rgb;
        //
        // DEVIATION (compile-technical, not a math change — DESIGN §2.2 gap):
        // this Sample() call is genuinely inside the else-branch, gated by a
        // per-fragment (non-uniform) trim/density condition. DESIGN §2.2's
        // "called unconditionally at the top, before any branch" note covers
        // ONLY the tex_trace sample above (HLSL:45); it does not extend to
        // THIS sample (HLSL:55), which sits inside the conditional exactly
        // as upstream wrote it. WGSL's derivative-uniformity rule forbids any
        // implicit-derivative textureSample() from non-uniform control flow
        // (confirmed by Tint at build time: "'textureSample' must only be
        // called from uniform control flow" pointing at this exact call) —
        // a hard compiler rejection, not a style choice, and D3D11/HLSL has
        // no equivalent restriction (Sample() there is unconditionally legal
        // anywhere). textureSampleLevel(..., 0.0) (explicit LOD, no implicit
        // derivatives) lifts the restriction and is numerically IDENTICAL
        // here: every texture this codebase creates (graphics.cpp's
        // get_texture2D/get_texture3D, both routed through make_texture) is
        // built with mipLevelCount == 1 (graphics.cpp:164,379) — with only
        // one mip level, implicit derivative-based LOD selection and an
        // explicit LOD of 0.0 select the exact same (only) level, so this
        // is not an approximation and not "fixing" upstream math, just a
        // language-level accommodation for a restriction D3D11 doesn't have.
        fragment = vec4<f32>(
            textureSampleLevel(tex_false_color, tex_false_color_sampler, vec2<f32>(remap(t, 1.0), 0.5), 0.0).rgb,
            // HLSL:56  fragment.a = optical_thickness * remap(t, 1.0);
            cfg.optical_thickness * remap(t, 1.0)
        );

        // HLSL:58-90  "Proxy draws config" — sightline/shell visualization
        // debug code, never compiled upstream (entirely commented out in
        // the HLSL source itself). Ported as a comment verbatim for 1:1
        // file correspondence (translation-notes.md T11 precedent: dead
        // code kept, not cleaned up).
        //
        // // Proxy draws config
        // // see http://geomalgorithms.com/a02-_lines.html
        //
        // // float3 o = float3(0.9321, 0.5179, 0.1403); // for SDSS_huge
        // // float3 v_l = float3(0.2175, 0.4660, 0.4807) - o; // for SDSS_huge
        //
        // // float3 o = float3(-0.0504, 1.0047, 0.8905); // for FRB_cigale
        // // float3 v_l = float3(0.6647, 0.4100, 0.4624) - o; // for FRB_cigale
        //
        // // float3 u_l = normalize(v_l);
        // // float3 w = input.texcoord_out.xyz - o;
        //
        // // // Proxy sightline
        // // float3 d = w - dot(w, u_l) * u_l;
        // // d *= float3(world_X, world_Y, world_Z);
        // // if (length(d) < 2.0) {
        // //     fragment.rgb += float3(2.0, t, 0.0);
        // //     fragment.a += optical_thickness * 2.0;
        // // }
        //
        // // // Proxy distance shells
        // // v_l *= float3(world_X, world_Y, world_Z);
        // // w *= float3(world_X, world_Y, world_Z);
        // // float roi_distance = 10.0 * galaxy_weight * length(v_l);
        // // float shell_thickness = 1.0; // voxels
        // // if (abs(length(w) - 0.25 * roi_distance) < shell_thickness ||
        // //     abs(length(w) - 0.50 * roi_distance) < shell_thickness ||
        // //     abs(length(w) - 0.75 * roi_distance) < shell_thickness ||
        // //     abs(length(w) - 1.00 * roi_distance) < shell_thickness ) {
        // //     fragment.rgb += float3(0.5, t, 1.0);
        // //     // fragment.rgb += float3(2.0, t, 0.2);
        // //     fragment.a += optical_thickness * 0.3;
        // // }
    }

    // HLSL:93  fragment.rgb *= 2.0; // Compensate for the drawing 1 stack of meta-quads instead of 3
    // QUIRK(single_stack_2x_compensation): kept for VAC parity — upstream draws 1 stack instead of 3
    fragment = vec4<f32>(fragment.rgb * 2.0, fragment.a);
    return fragment;
}
