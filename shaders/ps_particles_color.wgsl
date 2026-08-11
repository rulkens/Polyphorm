// ============================================================================
// ps_particles_color.wgsl
// Draft port of shaders/ps_particles_color.hlsl (Polyphorm, D3D11) to
// WGSL / Dawn.
//
// SPLIT NOTE: this file is the fragment stage half of the combined draft
// docs/superpowers/research/m3/wgsl-drafts/vs_2d_ps_particles_color.wgsl
// (M3 Task 2 — see vs_2d.wgsl's header for why the combined draft's single
// module is split into two files here: this fork's
// get_vertex_shader_from_code/get_pixel_shader_from_code (Task 1) each
// compile ONE file with ONE entry point named `main`). Content is otherwise
// IDENTICAL to the draft's fragment-stage section (lines 90-145) with only
// the entry point renamed fs_main -> main and the shared VertexOutput
// struct duplicated here (separate shader modules can't share WGSL source);
// all QUIRK comments (V7 unclamped exp highlight, V8 fixed teal ramp)
// preserved verbatim.
//
// STATUS: DRAFT port, now landed for compile validation (M3 Task 2). See
// translation-notes.md (M3, wgsl-drafts) for the full semantic difference
// table (§5, V1-V9).
//
// Draw call this backs (main.cpp:1140-1151, current M3-stub form):
//   graphics::set_vertex_shader(&vertex_shader_2d);
//   graphics::set_pixel_shader(&pixel_shader_2d);
//   graphics::set_texture(&display_tex, 0);
//   graphics::set_texture_sampler(&tex_sampler_display, 0);
//   graphics::draw_mesh(&quad_mesh);
//   graphics::unset_texture(0);
// ============================================================================

// ---------------------------------------------------------------------------
// Duplicated from vs_2d.wgsl: the fragment stage's input type. Separate
// shader modules cannot share WGSL source, so this struct is repeated
// verbatim here. The @location(0) texcoord_out field is the inter-module
// varying contract — it MUST match vs_2d.wgsl's VertexOutput.texcoord_out
// location exactly (both are location 0).
// ---------------------------------------------------------------------------

struct VertexOutput {
    // HLSL:9  float4 position_out: SV_POSITION;
    //   Unused by the fragment body below (kept only because it is part of
    //   the vertex stage's output struct, duplicated here for the shared
    //   type shape); WGSL permits @builtin(position) as a fragment-stage
    //   input (the rasterized fragment coordinate) even though this shader
    //   never reads it.
    @builtin(position) position_out: vec4<f32>,
    // HLSL:10  float2 texcoord_out: TEXCOORD;
    @location(0) texcoord_out: vec2<f32>,
};

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
//   kept as-is for grep-diffability against the HLSL. Slot 0 -> binding
//   2*0=0 (texture) / 2*0+1=1 (sampler), the 2N/2N+1 convention
//   (cpplib/graphics.cpp:741-759).
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
fn main(input: VertexOutput) -> @location(0) vec4<f32> {
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
