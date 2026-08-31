// cs_volpath_blit.wgsl — NEW SHADER, no HLSL counterpart (M4b design §2.6 option a).
// Copies the vec4 PT accumulator buffer into display_tex each PT frame, mirroring
// the cs_particles_transform -> cs_particles_blit two-dispatch shape. Exists because
// WGSL has no texture_storage_2d<rgba32float, read_write> for cs_volpath's original
// RWTexture2D<float4> read-modify-write (see cs_agents_propagate.wgsl:118's note).
// Dispatched exactly (window_width, window_height, 1) with 1x1x1 groups, matching
// cs_particles_blit's shape — gid is always in-bounds, no guard needed.
// Row stride == textureDimensions(...).x == window_width == the buffer's row stride
// (host keeps all three consistent; main.cpp recreates buffer + tex together on resize).

@group(1) @binding(0) var<storage, read> tex_accumulator : array<vec4<f32>>;
@group(1) @binding(1) var display_out : texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(1, 1, 1)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(display_out);
    let idx = gid.y * dims.x + gid.x;
    textureStore(display_out, vec2<i32>(i32(gid.x), i32(gid.y)), tex_accumulator[idx]);
}
