// ============================================================================
// vs_2d.wgsl
// Draft port of shaders/vs_2d.hlsl (Polyphorm, D3D11) to WGSL / Dawn.
//
// SPLIT NOTE: this file is the vertex stage half of the combined draft
// docs/superpowers/research/m3/wgsl-drafts/vs_2d_ps_particles_color.wgsl
// (M3 Task 2, per the plan's per-stage-module split — see that draft's V1:
// WGSL originally put both stages in one module; this fork's
// get_vertex_shader_from_code/get_pixel_shader_from_code (Task 1) each
// compile ONE file with ONE entry point named `main`, so vs/ps are split
// into separate files/modules here). Content is otherwise IDENTICAL to the
// draft's vertex-stage section (lines 56-88) with only the entry point
// renamed vs_main -> main (per the fork's `main`-only convention) and its
// struct comments retained verbatim.
//
// STATUS: DRAFT port, now landed for compile validation (M3 Task 2). See
// translation-notes.md (M3, wgsl-drafts) for the full semantic difference
// table (§5, V1-V2).
//
// Draw call this backs (main.cpp:1140-1151, current M3-stub form):
//   graphics::set_vertex_shader(&vertex_shader_2d);
//   graphics::set_pixel_shader(&pixel_shader_2d);
//   graphics::set_texture(&display_tex, 0);
//   graphics::set_texture_sampler(&tex_sampler_display, 0);
//   graphics::draw_mesh(&quad_mesh);
//   graphics::unset_texture(0);
// `quad_mesh` is 6 vertices (2 triangles, TRIANGLELIST, no index buffer),
// stride 6 floats == 24 bytes: float4 position (POSITION) + float2 texcoord
// (TEXCOORD), verified against main.cpp:328-345's `quad_vertices` /
// `quad_vertices_stride`.
// ============================================================================

// ---------------------------------------------------------------------------
// Vertex stage — port of shaders/vs_2d.hlsl (21 lines, pure passthrough).
// ---------------------------------------------------------------------------

struct VertexInput {
    // HLSL:3  float4 position: POSITION;
    @location(0) position: vec4<f32>,
    // HLSL:4  float2 texcoord: TEXCOORD;
    @location(1) texcoord: vec2<f32>,
};

struct VertexOutput {
    // HLSL:9  float4 position_out: SV_POSITION;
    @builtin(position) position_out: vec4<f32>,
    // HLSL:10  float2 texcoord_out: TEXCOORD;
    //   Fragment-stage `@location(0)` — WGSL requires an explicit numbered
    //   user attribute location for every non-builtin interstage value;
    //   HLSL's semantic name TEXCOORD carries no number so this is a
    //   mechanical assignment (there being exactly one interstage varying
    //   here makes the choice of 0 unambiguous). This location(0) is the
    //   inter-module contract with ps_particles_color.wgsl's fragment input
    //   — it must match the @location(0) texcoord there exactly.
    @location(0) texcoord_out: vec2<f32>,
};

// HLSL:13-21  VertexOutput main(VertexInput input) { result.position_out =
// input.position; result.texcoord_out = input.texcoord; return result; }
//   Direct, unconditional passthrough — no math, nothing to get wrong.
//   Entry point renamed vs_main -> main (this fork's single-entry-point-
//   per-module convention, cpplib/graphics.cpp:822 "ONE entry point named
//   `main`"); no other change from the combined draft.
@vertex
fn main(input: VertexInput) -> VertexOutput {
    var result: VertexOutput;
    result.position_out = input.position;   // HLSL:17
    result.texcoord_out = input.texcoord;   // HLSL:18
    return result;
}
