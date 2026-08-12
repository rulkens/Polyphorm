// ============================================================================
// vs_3d.wgsl
// Draft port of shaders/vs_3d.hlsl (Polyphorm, D3D11, 42 lines) to WGSL /
// Dawn. M4b Task 3.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed. See
// docs/superpowers/research/m4/m4b-volume-pt-design.md §2.1 for the
// translation plan this file implements.
//
// What this pass does: transforms a single-quad "super quad" (one of
// GRID_RESOLUTION stacked copies, `super_quad_mesh`, main.cpp:660-696) from
// model space into clip space, and passes through a texcoord remapped by
// `texcoord_map` — one of six axis-permutation branches selecting which
// world axis maps to which texcoord axis for the "most perpendicular to
// camera" slab orientation main.cpp computes per frame.
//
// Draw sites this backs (main.cpp:1298-1357, VM_VOLUME / VM_VOLUME_HIGHLIGHT
// / VM_VOLUME_HALOCOLOR / VM_VOLUME_OVERDENSITY / VM_VOLUME_VELOCITY):
//   graphics::set_vertex_shader(&vertex_shader);           // this file
//   graphics::set_texture(&trace_tex, 0);
//   graphics::set_texture_sampler(&tex_sampler_trace, 0);
//   graphics::set_pixel_shader(<one of the ps_volume_* variants>);
//   ... per-mode slot-1 texture/sampler ...
//   graphics::update_constant_buffer(&rendering_settings_buffer, &rendering_config);
//   graphics::set_constant_buffer(&rendering_settings_buffer, 0);
//   graphics::draw_mesh(&super_quad_mesh);
// `super_quad_mesh` is TRIANGLELIST, stride 7 floats == 28 bytes: float4
// position (POSITION) + float3 texcoord (TEXCOORD), verified against
// main.cpp:694-696's `super_quad_vertices_stride = sizeof(float) * 7`. This
// is the FIRST WGSL consumer of cpplib/graphics.cpp's `case 28:` vertex-
// layout branch (Float32x4 @ offset 0 loc 0, Float32x3 @ offset 16 loc 1) —
// present since M3 but never exercised by a real draw until this shader.
//
// Consumed by every ps_volume_* pixel shader (M4b Tasks 4/6/7) via the
// interstage contract below: @builtin(position) + @location(0) vec3<f32>
// texcoord.
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform (this stage has no @group(1); no
// textures/samplers are read in the vertex stage).
// ---------------------------------------------------------------------------

// HLSL:14-20  cbuffer ConfigBuffer : register(b4)
//   Per the M2b/M3 convention (carryover I3), b4-register cbuffers move to
//   @group(0) @binding(0) in this fork. The HLSL here declares only 3
//   matrices + texcoord_map (the first 1 of RenderingConfig's 36 scalars);
//   the full 336-byte struct is declared below anyway so ONE uniform buffer
//   + ONE @group(0) bind group can be shared with every other render/compute
//   pass (M2/M3 "declare the whole shared struct" rule, restated at DESIGN
//   §2 intro). Struct text copied verbatim from cs_particles_transform.wgsl
//   (M3) — see that file / translation-notes.md (M3) §1 for the full byte-
//   offset table and the matrix column-major cross-check (Matrix4x4 in
//   cpplib/maths.h:278-291 stores columns contiguously and defines M*v as
//   HLSL's mul(M,v) does; WGSL's mat4x4<f32> * vec4<f32> is the same
//   convention — no transpose needed anywhere in this port).
struct RenderingConfig {
    projection: mat4x4<f32>,               // +0    HLSL:16
    view: mat4x4<f32>,                     // +64   HLSL:17
    model: mat4x4<f32>,                    // +128  HLSL:18

    texcoord_map: i32,                     // +192  HLSL:19
    trim_x_min: f32,                       // +196  (unused here)
    trim_x_max: f32,                       // +200  (unused here)
    trim_y_min: f32,                       // +204  (unused here)

    trim_y_max: f32,                       // +208  (unused here)
    trim_z_min: f32,                       // +212  (unused here)
    trim_z_max: f32,                       // +216  (unused here)
    trim_density: f32,                     // +220  (unused here)

    world_width: f32,                      // +224  (unused here)
    world_height: f32,                     // +228  (unused here)
    world_depth: f32,                      // +232  (unused here)
    screen_width: f32,                     // +236  (unused here)

    screen_height: f32,                    // +240  (unused here)
    sample_weight: f32,                    // +244  (unused here)
    optical_thickness: f32,                // +248  (unused here)
    highlight_density: f32,                // +252  (unused here)

    galaxy_weight: f32,                    // +256  (unused here)
    histogram_base: f32,                   // +260  (unused here)
    overdensity_threshold_low: f32,        // +264  (unused here)
    overdensity_threshold_high: f32,       // +268  (unused here)

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

// ---------------------------------------------------------------------------
// Vertex stage — port of shaders/vs_3d.hlsl (42 lines).
// ---------------------------------------------------------------------------

struct VertexInput {
    // HLSL:3  float4 position: POSITION;
    @location(0) position: vec4<f32>,
    // HLSL:4  float3 texcoord: TEXCOORD;
    @location(1) texcoord: vec3<f32>,
};

struct VertexOutput {
    // HLSL:9  float4 position_out: SV_POSITION;
    @builtin(position) position_out: vec4<f32>,
    // HLSL:10  float3 texcoord_out: TEXCOORD;
    //   Interstage contract consumed by ALL ps_volume_* ports (M4b Tasks
    //   4/6/7) — this location(0) must match their fragment input exactly.
    @location(0) texcoord_out: vec3<f32>,
};

// HLSL:22-42  VertexOutput main(VertexInput input) { ... }
@vertex
fn main(input: VertexInput) -> VertexOutput {
    var result: VertexOutput;

    // HLSL:26  result.position_out = mul(projection_matrix, mul(view_matrix,
    //          mul(model_matrix, input.position)));
    //   Column-major matrix * column-vector, same convention on both sides
    //   (see the cbuffer comment above) — no transpose anywhere.
    result.position_out = cfg.projection * (cfg.view * (cfg.model * input.position));

    // HLSL:24  VertexOutput result;   (texcoord_out left uninitialized until
    //          a branch below assigns it)
    //   HLSL leaves texcoord_out undefined if none of the six branches match
    //   (main.cpp only ever sets texcoord_map to one of {+-1,+-2,+-3}, so
    //   this is unreachable in practice, but the HLSL itself has no defined
    //   fallback). WGSL requires every `var` to be initialized; zero-init is
    //   the closest defined reproduction of that unreachable-in-practice gap.
    var texcoord_out = vec3<f32>(0.0);

    // HLSL:27-39  six-branch texcoord_map permutation switch, ported
    //   literally (DESIGN §2.1 / spec line 103: "ported literally"). Each
    //   branch selects which world axis maps to which texcoord axis for one
    //   of the three "most perpendicular to camera" slab orientations
    //   main.cpp:1338-1357 computes.
    if (cfg.texcoord_map == 1) {
        // HLSL:28  float3(input.texcoord.x, input.texcoord.y, input.texcoord.z)
        texcoord_out = vec3<f32>(input.texcoord.x, input.texcoord.y, input.texcoord.z);
    } else if (cfg.texcoord_map == -1) {
        // HLSL:30  float3(1.0-input.texcoord.x, input.texcoord.y, 1.0-input.texcoord.z)
        texcoord_out = vec3<f32>(1.0 - input.texcoord.x, input.texcoord.y, 1.0 - input.texcoord.z);
    } else if (cfg.texcoord_map == 2) {
        // HLSL:32  float3(input.texcoord.x, input.texcoord.z, 1.0-input.texcoord.y)
        texcoord_out = vec3<f32>(input.texcoord.x, input.texcoord.z, 1.0 - input.texcoord.y);
    } else if (cfg.texcoord_map == -2) {
        // HLSL:34  float3(input.texcoord.x, 1.0-input.texcoord.z, input.texcoord.y)
        texcoord_out = vec3<f32>(input.texcoord.x, 1.0 - input.texcoord.z, input.texcoord.y);
    } else if (cfg.texcoord_map == 3) {
        // HLSL:36  float3(1.0-input.texcoord.z, input.texcoord.y, input.texcoord.x)
        texcoord_out = vec3<f32>(1.0 - input.texcoord.z, input.texcoord.y, input.texcoord.x);
    } else if (cfg.texcoord_map == -3) {
        // HLSL:38  float3(input.texcoord.z, input.texcoord.y, 1.0-input.texcoord.x)
        texcoord_out = vec3<f32>(input.texcoord.z, input.texcoord.y, 1.0 - input.texcoord.x);
    }

    result.texcoord_out = texcoord_out;

    return result;
}
