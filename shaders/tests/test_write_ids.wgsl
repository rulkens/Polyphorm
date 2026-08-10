override MULTIPLIER: u32 = 1u;

struct Cfg { mul : u32, add_ : u32, pad0 : u32, pad1 : u32 }
@group(0) @binding(0) var<uniform> cfg : Cfg;
@group(1) @binding(2) var<storage, read_write> out_buf : array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= arrayLength(&out_buf)) { return; }
    out_buf[gid.x] = gid.x * cfg.mul * MULTIPLIER + cfg.add_;
}
