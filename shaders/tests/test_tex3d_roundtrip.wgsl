@group(1) @binding(0) var tex : texture_storage_3d<r32float, read>;
@group(1) @binding(1) var<storage, read_write> out_buf : array<f32>;

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dims = textureDimensions(tex);
    if (gid.x >= dims.x || gid.y >= dims.y || gid.z >= dims.z) { return; }
    let idx = gid.x + gid.y * dims.x + gid.z * dims.x * dims.y;
    out_buf[idx] = textureLoad(tex, gid).x;
}
