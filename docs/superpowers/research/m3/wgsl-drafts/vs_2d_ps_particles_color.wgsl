// ============================================================================
// vs_2d_ps_particles_color.wgsl
// Combined vertex+fragment WGSL module: draft port of
//   shaders/vs_2d.hlsl            (vertex stage)
// + shaders/ps_particles_color.hlsl (fragment stage)
// for Polyphorm's VM_PARTICLES full-screen blit (Polyphorm, D3D11) to
// WGSL / Dawn. WGSL puts both stages in one shader module (single
// wgpu::ShaderModule, two entry points), unlike HLSL's separate .hlsl files
// compiled into separate VertexShader/PixelShader objects — hence one file
// for what was two HLSL sources.
//
// STATUS: DRAFT — not compiled. See translation-notes.md (M3, this directory).
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
// RENDER BIND-GROUP LAYOUT PROPOSAL (this pair needs none of it, but it is
// the shape every other M3/M4 render shader will need — documented here
// once so it is consistent everywhere; see translation-notes.md for the
// full rationale and the main.cpp call-site survey that constrains it.)
//
//   @group(0) @binding(0)      var<uniform> cfg: RenderingConfig   -- IF the
//                               shader pair has a cbuffer (this one does not:
//                               neither vs_2d.hlsl nor ps_particles_color.hlsl
//                               declare one).
//   @group(1) @binding(2*N)    texture bound via set_texture(_, N)
//   @group(1) @binding(2*N+1)  sampler bound via set_texture_sampler(_, N)
//
// Rationale: main.cpp's render-stage API takes a single `slot` for
// set_texture/set_texture_sampler (cpplib/graphics.h:116-119), called in
// PAIRS with the same slot number for texture+sampler at every VM_* call
// site (e.g. VM_VOLUME binds trace_tex+tex_sampler_trace at slot 0 and
// palette_trace_tex+tex_sampler_color_palette at slot 1 — main.cpp:1157-1163).
// HLSL keeps texture (tN) and sampler (sN) in separate register spaces, so a
// single WGSL @group(1) needs two distinct bindings per "slot" — doubling
// the slot number (texture at the even binding, its paired sampler at the
// odd one right after) keeps `binding == 2 * (the graphics.h slot argument)
// [+ 1 for the sampler]`, which is diffable against the call sites the same
// way `@group(1) @binding(N) == register(uN)` is diffable for compute
// (M2 notes §6). This is a genuinely NEW convention (compute has no sampler
// register space to double up against) — flagged as a proposal, not
// something already load-bearing elsewhere in the fork.
// ---------------------------------------------------------------------------

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
    //   here makes the choice of 0 unambiguous).
    @location(0) texcoord_out: vec2<f32>,
};

// HLSL:13-21  VertexOutput main(VertexInput input) { result.position_out =
// input.position; result.texcoord_out = input.texcoord; return result; }
//   Direct, unconditional passthrough — no math, nothing to get wrong.
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var result: VertexOutput;
    result.position_out = input.position;   // HLSL:17
    result.texcoord_out = input.texcoord;   // HLSL:18
    return result;
}

// ---------------------------------------------------------------------------
// Fragment stage — port of shaders/ps_particles_color.hlsl (18 lines).
// ---------------------------------------------------------------------------

// HLSL:7  Texture2D tex_trace : register(t0);
// HLSL:8  SamplerState tex_sampler_trace : register(s0);
//   Bound from main.cpp as (display_tex, tex_sampler_display) at slot 0 —
//   see the file header. Despite the HLSL name `tex_trace` (shared source
//   pattern with the volume shaders' actual trace texture), THIS instance
//   samples display_tex, the particle-splat accumulation target written by
//   cs_particles_blit.wgsl, not the 3D deposit trace field. Named `tex_trace`
//   here only because ps_particles_color.hlsl reuses that identifier locally;
//   kept as-is for grep-diffability against the HLSL.
@group(1) @binding(0) var tex_trace: texture_2d<f32>;
@group(1) @binding(1) var tex_sampler_trace: sampler;

// HLSL:10-18
//   float v = tex_trace.Sample(tex_sampler_trace, input.texcoord_out);
//     Implicit truncation: HLSL's Texture2D<float4>.Sample() (default
//     template arg) returns a float4; assigning it to `float v` silently
//     takes the FIRST component (.x / red) with a compiler warning. WGSL has
//     no implicit narrowing conversion, so this needs an explicit `.x`. This
//     is the same "which channel is meaningful" question flagged in
//     cs_particles_blit.wgsl's tex_out binding comment — v reads whatever
//     cs_particles_blit wrote as val_out, confirming that shader's red-
//     channel intent.
@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let v = textureSample(tex_trace, tex_sampler_trace, input.texcoord_out).x;   // HLSL:12

    // HLSL:13-15
    //   QUIRK(unclamped_exp_highlight): kept for parity. `1.0 - exp(-0.0001*v)`
    //   is a soft saturating curve for very bright (galaxy-splat, val >= 10000
    //   from the blit) pixels; there is no upper clamp, but exp(-x) -> 0 as
    //   x -> inf so the expression is naturally bounded in (0,1) for any
    //   finite non-negative v. No divide-by-zero or domain hazard here
    //   (unlike the acos/atan2 cases flagged in the M2 notes) — v can be
    //   negative only if sample_weight or the raw splat count were negative,
    //   which they structurally are not.
    if (v > 9999.0) {
        return vec4<f32>(1.0 - exp(-0.0001 * v), 0.0, 0.0, 1.0);   // HLSL:14
    }

    // HLSL:16-17
    //   QUIRK(fixed_teal_ramp): kept for parity. Below the galaxy threshold,
    //   color is a fixed (0.6, 1.0, 0.7)-weighted teal ramp of v/3, with NO
    //   clamp — v/3 > 1.0 (i.e. raw agent-splat val*sample_weight > 3.0)
    //   overshoots into an out-of-[0,1]-range, over-bright color that the
    //   swap-chain's implicit clamp-on-present silently clips. This is the
    //   render-stage's OOB-clamp analogue of the compute-stage OOB-load/
    //   store questions elsewhere in this port: harmless here (clamp is the
    //   correct, intended behaviour for a color output), called out only so
    //   it isn't mistaken for a bug during an M5 visual diff.
    let v_scaled = v / 3.0;                                          // HLSL:16
    return vec4<f32>(0.6 * v_scaled, v_scaled, 0.7 * v_scaled, 1.0);  // HLSL:17
}
