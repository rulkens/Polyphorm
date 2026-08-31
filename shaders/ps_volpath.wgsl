// ============================================================================
// ps_volpath.wgsl
// Draft port of shaders/ps_volpath.hlsl (Polyphorm, D3D11, 59 lines) to
// WGSL / Dawn. M4b Task 10.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed. See
// docs/superpowers/research/m4/m4b-volume-pt-design.md §2.7 for the
// translation plan this file implements.
//
// What this pass does: displays the path-traced (PT) accumulator — this
// samples the display texture that cs_volpath_blit.wgsl (M4b Task 8) wrote
// the buffer-averaged radiance into — and applies a final tonemap/gamma-ish
// display transform (or passes the raw radiance through, when
// compressive_accumulation is already applied upstream in the accumulator).
//
// Pairs with the already-ported vs_2d.wgsl (shaders/vs_2d.wgsl, M3) — this
// pixel shader is the only new file needed here, no new vertex shader.
//
// Draw site this backs (main.cpp:1399-1407, VM_PATH_TRACING):
//   graphics::set_vertex_shader(&vertex_shader_2d);         // vs_2d.wgsl
//   graphics::set_texture(&display_tex, 0);
//   graphics::set_texture_sampler(&tex_sampler_display, 0);
//   graphics::set_pixel_shader(&pixel_shader);               // this file
//   ... update/set_constant_buffer(&rendering_settings_buffer, 0) ...
//   graphics::draw_mesh(&quad_mesh);
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) render-stage textures.
// ---------------------------------------------------------------------------

// HLSL:9-50  cbuffer ConfigBuffer : register(b4)
//   Per the M2b/M3/M4b convention (carryover I3), b4-register cbuffers move
//   to @group(0) @binding(0) in this fork. Struct text copied verbatim from
//   ps_volume_trace.wgsl / vs_3d.wgsl (M3/M4b) — see translation-notes.md
//   (M3) §1 for the full byte-offset table and the matrix column-major
//   cross-check (no transpose needed anywhere).
//
//   NAMING NOTE (DESIGN §2.7, not a bug): HLSL:48-49 name the struct's last
//   two members `dummy2`/`dummy3` (int) instead of the canonical
//   RenderingConfig names `guiding_strength`/`scattering_anisotropy`
//   (float) — same byte offsets (+328/+332), unused by this shader either
//   way. This port uses the canonical RenderingConfig field names/types
//   per the shared-struct convention (§2 intro / adjudication 4), matching
//   every other WGSL port of this shared struct, not the HLSL's
//   locally-inconsistent names.
//
//   THIS SHADER DECLARES A CBUFFER (HLSL:9-50) — so it *does* need
//   @group(0), even though its paired vertex shader (vs_2d.wgsl) does not
//   (vs_2d.wgsl has no cbuffer, translation-notes V9). This is the "fourth"
//   missing set_constant_buffer site DESIGN §5 flags for main.cpp
//   (VM_PATH_TRACING's draw_mesh at main.cpp:1405) — wiring is Task 11, not
//   touched here.
struct RenderingConfig {
    projection: mat4x4<f32>,               // +0    HLSL:11
    view: mat4x4<f32>,                     // +64   HLSL:12
    model: mat4x4<f32>,                    // +128  HLSL:13

    texcoord_map: i32,                     // +192  HLSL:14  (unused here)
    trim_x_min: f32,                       // +196  HLSL:15  (unused here)
    trim_x_max: f32,                       // +200  HLSL:16  (unused here)
    trim_y_min: f32,                       // +204  HLSL:17  (unused here)

    trim_y_max: f32,                       // +208  HLSL:18  (unused here)
    trim_z_min: f32,                       // +212  HLSL:19  (unused here)
    trim_z_max: f32,                       // +216  HLSL:20  (unused here)
    trim_density: f32,                     // +220  HLSL:21  (unused here)

    world_width: f32,                      // +224  HLSL:22  (grid_x — unused here)
    world_height: f32,                     // +228  HLSL:23  (grid_y — unused here)
    world_depth: f32,                      // +232  HLSL:24  (grid_z — unused here)
    screen_width: f32,                     // +236  HLSL:25  (unused here)

    screen_height: f32,                    // +240  HLSL:26  (unused here)
    sample_weight: f32,                    // +244  HLSL:27  (unused here)
    optical_thickness: f32,                // +248  HLSL:28  (unused here)
    highlight_density: f32,                // +252  HLSL:29  (unused here)

    galaxy_weight: f32,                    // +256  HLSL:30  (unused here)
    histogram_base: f32,                   // +260  HLSL:31  (unused here)
    overdensity_threshold_low: f32,        // +264  HLSL:32  (unused here)
    overdensity_threshold_high: f32,       // +268  HLSL:33  (unused here)

    camera_x: f32,                         // +272  HLSL:34  (unused here)
    camera_y: f32,                         // +276  HLSL:35  (unused here)
    camera_z: f32,                         // +280  HLSL:36  (unused here)
    pt_iteration: i32,                     // +284  HLSL:37  (unused here)

    sigma_s: f32,                          // +288  HLSL:38  (unused here)
    sigma_a: f32,                          // +292  HLSL:39  (unused here)
    sigma_e: f32,                          // +296  HLSL:40  (unused here)
    trace_max: f32,                        // +300  HLSL:41  (unused here)

    camera_offset_x: f32,                  // +304  HLSL:42  (unused here)
    camera_offset_y: f32,                  // +308  HLSL:43  (unused here)
    exposure: f32,                         // +312  HLSL:44
    n_bounces: i32,                        // +316  HLSL:45  (unused here)

    ambient_trace: f32,                    // +320  HLSL:46  (unused here)
    compressive_accumulation: i32,         // +324  HLSL:47
    guiding_strength: f32,                 // +328  HLSL:48  (HLSL: `int dummy2` — see naming note above; unused here)
    scattering_anisotropy: f32,            // +332  HLSL:49  (HLSL: `int dummy3` — see naming note above; unused here)
};
// sizeof == 336, matches main.cpp:313's static_assert exactly.
@group(0) @binding(0) var<uniform> cfg: RenderingConfig;

// HLSL:6-7  Texture2D tex : register(t0); SamplerState tex_sampler : register(s0);
//   Render bind-group convention: binding == 2*slot for the texture,
//   2*slot+1 for its paired sampler, where slot is the argument to
//   graphics::set_texture/set_texture_sampler (translation-notes.md §6).
@group(1) @binding(0) var tex: texture_2d<f32>;
@group(1) @binding(1) var tex_sampler: sampler;

// HLSL:1-4  PixelInput — this shader's fragment input is vs_2d.wgsl's
// VertexOutput (interstage contract: @builtin(position) + @location(0)
// vec2<f32> texcoord_out — must match vs_2d.wgsl exactly).
struct PixelInput {
    @builtin(position) position_out: vec4<f32>,
    @location(0) texcoord_out: vec2<f32>,
};

// HLSL:52-54
fn tonemap(L: vec3<f32>, exposure: f32) -> vec3<f32> {
    return vec3<f32>(1.0) - exp(-exposure * L);
}

// HLSL:56-59
@fragment
fn main(input: PixelInput) -> @location(0) vec4<f32> {
    // HLSL:57  float3 L = tex.Sample(tex_sampler, input.texcoord_out).rgb;
    //   Called unconditionally before any branch — plain textureSample()
    //   (no derivative-uniformity concern, same shape as
    //   ps_volume_trace.wgsl's HLSL:45 unconditional-sample precedent).
    let L = textureSample(tex, tex_sampler, input.texcoord_out).rgb;
    // HLSL:58  return float4(compressive_accumulation == 1 ? L : tonemap(L, exposure), 1.0);
    //   HLSL's ternary becomes select() here. select(f, t, cond) picks `t`
    //   when cond is true; both arms are evaluated either way (no
    //   short-circuiting in WGSL, same as HLSL's ?: for scalar/vector
    //   operands here) — safe because both tonemap(L, exposure) and L are
    //   pure (no side effects), so evaluating both is not observable.
    return vec4<f32>(select(tonemap(L, cfg.exposure), L, cfg.compressive_accumulation == 1), 1.0);
}
