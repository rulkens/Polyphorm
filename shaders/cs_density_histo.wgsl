// Port of cs_density_histo.hlsl — bins the trace density at data-point
// positions into a log histogram. Bug-for-bug: RNG constants and the
// seed-guard typo (m_w tested where m_z is meant) are preserved verbatim.
//
// Bind contract (see graphics:: convention):
//   @group(0) @binding(0)  StatisticsConfig uniform (32 B)
//   @group(1) @binding(0)  trace texture   (r32float, read_write — matches RWTexture3D)
//   @group(1) @binding(1)  histogram       (atomic<u32> storage)
//   @group(1) @binding(2..4) particles x/y/z (f32 storage)
//   @group(1) @binding(5)  particles_weights
//   @group(1) @binding(6)  halos_densities

override WG_X: u32 = 10u;
override WG_Y: u32 = 10u;
override WG_Z: u32 = 10u;

struct StatisticsConfig {
    n_data_points: i32,
    n_histo_bins: i32,
    histogram_base: f32,
    sample_randomly: i32,
    world_width: f32,
    world_height: f32,
    world_depth: f32,
    filler3: i32,
};

@group(0) @binding(0) var<uniform> cfg: StatisticsConfig;

@group(1) @binding(0) var tex_density: texture_storage_3d<r32float, read_write>;
@group(1) @binding(1) var<storage, read_write> histogram: array<atomic<u32>>;
@group(1) @binding(2) var<storage, read_write> particles_x: array<f32>;
@group(1) @binding(3) var<storage, read_write> particles_y: array<f32>;
@group(1) @binding(4) var<storage, read_write> particles_z: array<f32>;
@group(1) @binding(5) var<storage, read_write> particles_weights: array<f32>;
@group(1) @binding(6) var<storage, read_write> halos_densities: array<f32>;

const BAD_W: u32 = 0x464fffffu;
const BAD_Z: u32 = 0x9068ffffu;

struct RNG { m_w: u32, m_z: u32, };

fn rng_set_seed(rng: ptr<function, RNG>, seed1: u32, seed2: u32) {
    (*rng).m_w = seed1;
    (*rng).m_z = seed2;
    // QUIRK(rng_seed_guard_typo): second guard tests m_w where m_z is meant,
    // exactly as the HLSL does. A zero m_z is never repaired.
    if ((*rng).m_w == 0u || (*rng).m_w == BAD_W) { (*rng).m_w = (*rng).m_w + 1u; }
    if ((*rng).m_w == 0u || (*rng).m_z == BAD_Z) { (*rng).m_z = (*rng).m_z + 1u; }
}

fn rng_random_uint(rng: ptr<function, RNG>) -> u32 {
    (*rng).m_z = 36969u * ((*rng).m_z & 65535u) + ((*rng).m_z >> 16u);
    (*rng).m_w = 18000u * ((*rng).m_w & 65535u) + ((*rng).m_w >> 16u);
    return ((*rng).m_z << 16u) + (*rng).m_w;
}

fn rng_random_float(rng: ptr<function, RNG>) -> f32 {
    return f32(rng_random_uint(rng)) / f32(0xFFFFFFFFu);
}

fn wang_hash(seed_in: u32) -> u32 {
    var seed = (seed_in ^ 61u) ^ (seed_in >> 16u);
    seed = seed * 9u;
    seed = seed ^ (seed >> 4u);
    seed = seed * 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

@compute @workgroup_size(WG_X, WG_Y, WG_Z)
fn main(@builtin(local_invocation_index) thread_index: u32,
        @builtin(workgroup_id) group_id: vec3<u32>,
        @builtin(num_workgroups) num_groups: vec3<u32>) {
    // Generalised flat index; reduces to thread_index + 1000*(gx + gy*10 + gz*100)
    // for the production dispatch (10,10,gz) — same bijection as the HLSL.
    let group_idx = group_id.x + group_id.y * num_groups.x + group_id.z * num_groups.x * num_groups.y;
    let idx = thread_index + (WG_X * WG_Y * WG_Z) * group_idx;

    if (idx >= u32(cfg.n_data_points)) { // Only consider halo/galaxy locations
        return;
    }

    var x = particles_x[idx];
    var y = particles_y[idx];
    var z = particles_z[idx];

    if (cfg.sample_randomly != 0) {
        var rng: RNG;
        rng_set_seed(&rng, wang_hash(73u * idx), wang_hash(u32(x * y * z)));
        x = rng_random_float(&rng) * cfg.world_width;
        y = rng_random_float(&rng) * cfg.world_height;
        z = rng_random_float(&rng) * cfg.world_depth;
    }

    let density = textureLoad(tex_density, vec3<i32>(vec3<u32>(vec3<f32>(x, y, z)))).x;
    halos_densities[idx] = density;

    var histo_index = 0u;
    if (density > 1.0e-5) {
        let log_density = log(density) / log(cfg.histogram_base);
        // f32->u32 saturates in WGSL; D3D GPU hardware saturates too — matching.
        histo_index = 1u + min(u32(log_density + 5.0), u32(cfg.n_histo_bins - 3));
    }

    atomicAdd(&histogram[histo_index], 1u);
    atomicMax(&histogram[u32(cfg.n_histo_bins - 1)], u32(1.0e5 * density));
}
