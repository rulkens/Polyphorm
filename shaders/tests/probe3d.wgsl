// Test-only: probe voxels of a 3D r32float texture and sum one x-slab.
struct ProbeCfg {
    n_probes: i32,
    slab_x: i32,      // x index of the slab to sum; -1 = skip
    dim_y: i32,
    dim_z: i32,
};
@group(0) @binding(0) var<uniform> cfg: ProbeCfg;
@group(1) @binding(0) var tex: texture_storage_3d<r32float, read_write>;
@group(1) @binding(1) var<storage, read_write> coords: array<i32>;   // xyz triples
@group(1) @binding(2) var<storage, read_write> results: array<f32>;  // n_probes values + [n_probes] = slab sum

@compute @workgroup_size(1, 1, 1)
fn main() {
    for (var i = 0; i < cfg.n_probes; i = i + 1) {
        let c = vec3<i32>(coords[3*i], coords[3*i+1], coords[3*i+2]);
        results[i] = textureLoad(tex, c).x;
    }
    if (cfg.slab_x >= 0) {
        var sum = 0.0;
        for (var y = 0; y < cfg.dim_y; y = y + 1) {
            for (var z = 0; z < cfg.dim_z; z = z + 1) {
                sum = sum + textureLoad(tex, vec3<i32>(cfg.slab_x, y, z)).x;
            }
        }
        results[cfg.n_probes] = sum;
    }
}
