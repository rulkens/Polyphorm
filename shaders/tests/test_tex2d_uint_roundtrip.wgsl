@group(1) @binding(0) var tex : texture_storage_2d<r32uint, read>;
@group(1) @binding(1) var<storage, read_write> out_buf : array<u32>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(tex);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }
    let idx = gid.x + gid.y * dims.x;
    out_buf[idx] = textureLoad(tex, gid.xy).x;
}
