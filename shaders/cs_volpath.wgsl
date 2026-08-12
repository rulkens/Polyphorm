// ============================================================================
// cs_volpath.wgsl
// Port of shaders/cs_volpath.hlsl (Polyphorm, D3D11, 455 lines) to WGSL /
// Dawn. M4b Task 8 — the volumetric path tracer's compute half.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed. See
// docs/superpowers/research/m4/m4b-volume-pt-design.md §2.6 for the
// accumulator-conversion rationale this file implements, and
// .superpowers/sdd/2026-08-12-macos-port-m4b-volume-pt/task-8-brief.md for
// the pinned adjudications (binding table, OOB guard, QUIRK list).
//
// ACCUMULATOR CONVERSION (DESIGN §2.6, adjudicated — not reopened here):
//   HLSL's `RWTexture2D<float4> tex_accumulator` is read AND written in the
//   same invocation (the TEMPORAL_ACCUMULATION running average). WGSL has no
//   texture_storage_2d<rgba32float, read_write> (read_write storage textures
//   are restricted to r32uint/r32sint/r32float — see cs_agents_propagate.wgsl
//   :118's note, independently confirmed here). tex_accumulator therefore
//   becomes a `var<storage, read_write> array<vec4<f32>>` buffer, row-major
//   `y*width+x`, at @group(1) @binding(0). Plain non-atomic read-modify-write
//   is exact here: each invocation owns exactly one pixel, no cross-thread
//   collisions. A new sibling shader, cs_volpath_blit.wgsl (no HLSL
//   counterpart), copies this buffer into display_tex each frame, mirroring
//   the cs_particles_transform -> cs_particles_blit two-dispatch shape.
//
// COMPUTE-STAGE SAMPLING NOTE: every texture sample in this file uses
// textureSampleLevel (explicit LOD 0.0), not textureSample — compute shaders
// have no fragment derivatives, so textureSample is unavailable here by
// construction. This is the norm for ALL SampleLevel-derived calls in this
// file (the HLSL already used SampleLevel(..., 0) everywhere, never the
// derivative-based Sample()), not a per-site deviation requiring individual
// justification.
//
// QUIRK INVENTORY (full list; each also marked inline at its site):
//   - QUIRK(set_seed_mw_recheck)      HLSL:85   RNG.set_seed's second guard
//   - QUIRK(seed_idx_truncation)      HLSL:351  uint3->uint truncation
//   - QUIRK(sky_scalar_truncation)    HLSL:203  get_sky_L float/float3 mismatch
//   - QUIRK(oob_dispatch_guard)       main()    NEW guard, no HLSL counterpart
//   - dead code kept for 1:1 correspondence (M3 T11 precedent, not "quirks"
//     in the buggy-math sense, but preserved verbatim): get_seed,
//     ray_sphere_intersection (stray `;` dropped), occlusion_tracking,
//     uniform_unit_sphere / get_halo_gradient (live but reachable only via
//     the disabled GRADIENT_GUIDING/POINT_ILLUMINATION comment blocks).
//
// FEATURE FLAGS: HLSL's #define-gated feature set has no WGSL preprocessor
// equivalent. Per DESIGN §2.6 / the task brief, the shipping set is
// hardcoded directly into the control flow (TEMPORAL_ACCUMULATION,
// RUSSIAN_ROULETTE, HALO_ILLUMINATION, TRACE_ILLUMINATION — all #define'd ON
// at HLSL:11,12,19,20); the disabled set (GRADIENT_GUIDING, TRACE_SHARPENING,
// WHITESKY_ILLUMINATION, POINT_ILLUMINATION — all commented out at
// HLSL:13,14,17,18) is kept as clearly-marked comment blocks reading
// `// #ifdef <NAME> (disabled in default build) — kept for 1:1
// correspondence:`, same treatment as M3's dead RNG code.
//   Note: TRACE_SHARPENING is `#define`d at HLSL:14 but never referenced by
//   any #ifdef in the 455-line body — dead-on-arrival in the HLSL itself;
//   nothing to port for it beyond this note.
// ============================================================================

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) compute-pass resources.
// Compute convention: binding == HLSL register index; samplers at 16+slot
// (m4a-carryovers.md / DESIGN §1, confirmed against cpplib/graphics.cpp:
// 1071-1080's MAX_SLOTS(16)+slot scheme).
// ---------------------------------------------------------------------------

// HLSL:32-73  cbuffer ConfigBuffer : register(b4)
//   Full 336-byte RenderingConfig, canonical field names (world_width/
//   world_height/world_depth — this HLSL's own `grid_x/grid_y/grid_z` names
//   are the ones DESIGN §2 intro calls the more-accurate alias; every WGSL
//   port of this shared struct uses the canonical names). Struct text
//   matches vs_3d.wgsl / ps_volume_trace.wgsl verbatim.
struct RenderingConfig {
    projection: mat4x4<f32>,               // +0    HLSL:34  (unused here)
    view: mat4x4<f32>,                     // +64   HLSL:35  (unused here)
    model: mat4x4<f32>,                    // +128  HLSL:36  (unused here)

    texcoord_map: i32,                     // +192  HLSL:37  (unused here)
    trim_x_min: f32,                       // +196  HLSL:38  (used: c_low_trimmed)
    trim_x_max: f32,                       // +200  HLSL:39  (used: c_high_trimmed)
    trim_y_min: f32,                       // +204  HLSL:40  (used: c_low_trimmed)

    trim_y_max: f32,                       // +208  HLSL:41  (used: c_high_trimmed)
    trim_z_min: f32,                       // +212  HLSL:42  (used: c_low_trimmed)
    trim_z_max: f32,                       // +216  HLSL:43  (used: c_high_trimmed)
    trim_density: f32,                     // +220  HLSL:44  (used: trace_to_rho)

    world_width: f32,                      // +224  HLSL:45  (grid_x — used: get_rho/get_halo, grid_res)
    world_height: f32,                     // +228  HLSL:46  (grid_y — used)
    world_depth: f32,                      // +232  HLSL:47  (grid_z — used)
    screen_width: f32,                     // +236  HLSL:48  (used: OOB guard, pix_idx, aspect_ratio, rx)

    screen_height: f32,                    // +240  HLSL:49  (used: OOB guard, aspect_ratio, ry)
    sample_weight: f32,                    // +244  HLSL:50  (used: trace_to_rho)
    optical_thickness: f32,                // +248  HLSL:51  (unused here — ps_volume_trace only)
    highlight_density: f32,                // +252  HLSL:52  (unused here)

    galaxy_weight: f32,                    // +256  HLSL:53  (used: get_halo)
    histogram_base: f32,                   // +260  HLSL:54  (unused here)
    overdensity_threshold_low: f32,        // +264  (not in this HLSL cbuffer; unused here)
    overdensity_threshold_high: f32,       // +268  (not in this HLSL cbuffer; unused here)

    camera_x: f32,                         // +272  HLSL:57  (used: main camera_pos)
    camera_y: f32,                         // +276  HLSL:58  (used)
    camera_z: f32,                         // +280  HLSL:59  (used)
    pt_iteration: i32,                     // +284  HLSL:60  (used: zero-fill gate, RNG seed, temporal accum)

    sigma_s: f32,                          // +288  HLSL:61  (used: albedo, sigma_max_inv, scattering gate)
    sigma_a: f32,                          // +292  HLSL:62  (used)
    sigma_e: f32,                          // +296  HLSL:63  (used: emission scaling)
    trace_max: f32,                        // +300  HLSL:64  (used: rho_max_inv)

    camera_offset_x: f32,                  // +304  HLSL:65  (used: screen_pos)
    camera_offset_y: f32,                  // +308  HLSL:66  (used: screen_pos)
    exposure: f32,                         // +312  HLSL:67  (used: tonemap)
    n_bounces: i32,                        // +316  HLSL:68  (used: get_incident_L bounce count)

    ambient_trace: f32,                    // +320  HLSL:69  (used: trace_to_rho)
    compressive_accumulation: i32,         // +324  HLSL:70  (used: tonemap gate)
    guiding_strength: f32,                 // +328  HLSL:71  (unused in active control flow — only in the
                                            //                disabled GRADIENT_GUIDING comment block)
    scattering_anisotropy: f32,            // +332  HLSL:72  (used: sample_HG anisotropy)
};
// sizeof == 336, matches main.cpp:313's static_assert exactly.
@group(0) @binding(0) var<uniform> cfg: RenderingConfig;

// HLSL:22  RWTexture2D<float4> tex_accumulator: register(u0);
//   See the ACCUMULATOR CONVERSION header note above — buffer, not texture,
//   row-major y*width+x. Plain (non-atomic) read_write: exact here, one
//   invocation per pixel, no collisions.
@group(1) @binding(0) var<storage, read_write> tex_accumulator: array<vec4<f32>>;

// HLSL:23-24  Texture3D tex_trace : register(t1); SamplerState tex_trace_sampler : register(s1);
@group(1) @binding(1) var tex_trace: texture_3d<f32>;
@group(1) @binding(17) var tex_trace_sampler: sampler;

// HLSL:25-26  Texture3D tex_deposit : register(t2); SamplerState tex_deposit_sampler : register(s2);
@group(1) @binding(2) var tex_deposit: texture_3d<f32>;
@group(1) @binding(18) var tex_deposit_sampler: sampler;

// HLSL:27-28  Texture2D tex_palette_trace : register(t3); SamplerState tex_palette_trace_sampler : register(s3);
@group(1) @binding(3) var tex_palette_trace: texture_2d<f32>;
@group(1) @binding(19) var tex_palette_trace_sampler: sampler;

// HLSL:29-30  Texture2D tex_palette_data : register(t4); SamplerState tex_palette_data_sampler : register(s4);
@group(1) @binding(4) var tex_palette_data: texture_2d<f32>;
@group(1) @binding(20) var tex_palette_data_sampler: sampler;

// ---------------------------------------------------------------------------
// Constants (HLSL:1-8) — truncated-PI convention preserved verbatim, per
// house style (translation-notes.md): do NOT substitute a more precise pi.
// ---------------------------------------------------------------------------

const PI: f32 = 3.141592;              // HLSL:3  NOTE: truncated PI, kept verbatim
const PI2: f32 = 6.283184;             // HLSL:4
const INV_PI4: f32 = 0.079577;         // HLSL:5  unused in HLSL and here; kept for 1:1 correspondence
const RAY_EPSILON: f32 = 1e-5;         // HLSL:6
const INTENSITY_EPSILON: f32 = 1e-4;   // HLSL:7
const NUMERICAL_EPSILON: f32 = 1e-4;   // HLSL:8

// ---------------------------------------------------------------------------
// RNG (HLSL:75-111 `struct RNG` with member functions). WGSL structs have no
// methods; these become free functions taking `ptr<function, RNG>`, same
// shape as cs_agents_propagate.wgsl's RNG port.
// ---------------------------------------------------------------------------

const BAD_W: u32 = 0x464fffffu;   // HLSL:76 #define BAD_W
const BAD_Z: u32 = 0x9068ffffu;   // HLSL:77 #define BAD_Z

struct RNG {
    m_w: u32,   // HLSL:78
    m_z: u32,   // HLSL:79
};

// HLSL:81-86  void set_seed(uint seed1, uint seed2)
fn rng_set_seed(rng: ptr<function, RNG>, seed1: u32, seed2: u32) {
    (*rng).m_w = seed1;                                       // HLSL:82
    (*rng).m_z = seed2;                                       // HLSL:83
    if ((*rng).m_w == 0u || (*rng).m_w == BAD_W) {             // HLSL:84
        (*rng).m_w = (*rng).m_w + 1u;
    }
    // QUIRK(set_seed_mw_recheck): the second guard re-checks m_w == 0 (HLSL
    // :85 — likely meant m_z == 0). Preserved verbatim.
    if ((*rng).m_w == 0u || (*rng).m_z == BAD_Z) {             // HLSL:85
        (*rng).m_z = (*rng).m_z + 1u;
    }
}

// HLSL:88-91  void get_seed(out uint seed1, out uint seed2) — dead code,
// never called from main(). Ported as a real (but unreferenced) function
// for 1:1 file correspondence (M3 translation-notes T11 precedent).
fn rng_get_seed(rng: ptr<function, RNG>, seed1: ptr<function, u32>, seed2: ptr<function, u32>) {
    (*seed1) = (*rng).m_w;
    (*seed2) = (*rng).m_z;
}

// HLSL:93-97  uint random_uint()
fn rng_random_uint(rng: ptr<function, RNG>) -> u32 {
    (*rng).m_z = 36969u * ((*rng).m_z & 65535u) + ((*rng).m_z >> 16u);  // HLSL:94
    (*rng).m_w = 18000u * ((*rng).m_w & 65535u) + ((*rng).m_w >> 16u);  // HLSL:95
    return ((*rng).m_z << 16u) + (*rng).m_w;                            // HLSL:96
}

// HLSL:99-101  float random_float()
fn rng_random_float(rng: ptr<function, RNG>) -> f32 {
    return f32(rng_random_uint(rng)) / f32(0xFFFFFFFFu);                // HLSL:100
}

// HLSL:103-110  uint wang_hash(uint seed) — stateless (reads no RNG
// members), so a free function is exact, same as cs_agents_propagate.wgsl.
fn wang_hash(seed_in: u32) -> u32 {
    var seed = seed_in;
    seed = (seed ^ 61u) ^ (seed >> 16u);   // HLSL:104
    seed = seed * 9u;                      // HLSL:105
    seed = seed ^ (seed >> 4u);            // HLSL:106
    seed = seed * 0x27d4eb2du;             // HLSL:107
    seed = seed ^ (seed >> 15u);           // HLSL:108
    return seed;                           // HLSL:109
}

// ---------------------------------------------------------------------------
// Helpers (HLSL:113-345)
// ---------------------------------------------------------------------------

// HLSL:113-124  float3 uniform_unit_sphere(inout RNG rng) — reachable only
// via the disabled GRADIENT_GUIDING branch inside get_incident_L (its call
// site, HLSL:338, is kept as a comment there); ported as a live function for
// 1:1 correspondence, same treatment as the rest of the disabled-feature code.
fn uniform_unit_sphere(rng: ptr<function, RNG>) -> vec3<f32> {
    let azimuth = rng_random_float(rng) * PI2;                          // HLSL:114
    let polar = acos(2.0 * rng_random_float(rng) - 1.0);                // HLSL:115
    let r = pow(rng_random_float(rng), 1.0 / 3.0);                      // HLSL:116

    let result = vec3<f32>(
        r * cos(azimuth) * sin(polar),
        r * cos(polar),
        r * sin(azimuth) * sin(polar)
    );                                                                  // HLSL:118-122
    return result;                                                      // HLSL:123
}

// HLSL:126-144  float ray_sphere_intersection(...) — dead code, never called
// from main(). Ported verbatim for 1:1 file correspondence. The stray `;`
// after the closing brace at HLSL:144 is dropped: WGSL rejects a top-level
// `;` immediately following a function definition.
fn ray_sphere_intersection(rp: vec3<f32>, rd: vec3<f32>, p: vec3<f32>, r: f32) -> f32 {
    let os = rp - p;                                                    // HLSL:127
    let a = dot(rd, rd);                                                // HLSL:128
    let b = 2.0 * dot(os, rd);                                          // HLSL:129
    let c = dot(os, os) - r * r;                                        // HLSL:130
    let discriminant = b * b - 4.0 * a * c;                             // HLSL:131

    if (discriminant > 0.0) {                                           // HLSL:133
        var t = (-b - sqrt(discriminant)) / (2.0 * a);                  // HLSL:134
        if (t > 0.0001) {                                                // HLSL:135
            return t;                                                    // HLSL:136
        }
        t = (-b + sqrt(discriminant)) / (2.0 * a);                      // HLSL:138
        if (t > 0.0001) {                                                // HLSL:139
            return t;                                                    // HLSL:140
        }
    }
    return -1.0;                                                        // HLSL:143
}

// HLSL:146-157  float2 ray_AABB_intersection(...)
fn ray_AABB_intersection(rp: vec3<f32>, rd: vec3<f32>, c_lo: vec3<f32>, c_hi: vec3<f32>) -> vec2<f32> {
    var t: array<f32, 8>;                                               // HLSL:147
    t[0] = (c_lo.x - rp.x) / rd.x;                                      // HLSL:148
    t[1] = (c_hi.x - rp.x) / rd.x;                                      // HLSL:149
    t[2] = (c_lo.y - rp.y) / rd.y;                                      // HLSL:150
    t[3] = (c_hi.y - rp.y) / rd.y;                                      // HLSL:151
    t[4] = (c_lo.z - rp.z) / rd.z;                                      // HLSL:152
    t[5] = (c_hi.z - rp.z) / rd.z;                                      // HLSL:153
    t[6] = max(max(min(t[0], t[1]), min(t[2], t[3])), min(t[4], t[5]));  // HLSL:154
    t[7] = min(min(max(t[0], t[1]), max(t[2], t[3])), max(t[4], t[5]));  // HLSL:155
    // Division by a zero rd component yields inf identically in both APIs
    // (IEEE-754 float division) — no change needed, one-line note only.
    if (t[7] < 0.0 || t[6] >= t[7]) {                                    // HLSL:156
        return vec2<f32>(-1.0, -1.0);
    }
    return vec2<f32>(t[6], t[7]);
}

// HLSL:159-164  float3 coord_normalized_to_texture(...)
fn coord_normalized_to_texture(coord: vec3<f32>, c_lo: vec3<f32>, c_hi: vec3<f32>, size: vec3<f32>) -> vec3<f32> {
    var coord_rel = (coord - c_lo) / (c_hi - c_lo);                     // HLSL:160
    coord_rel.y = 1.0 - coord_rel.y;                                    // HLSL:161
    coord_rel.z = 1.0 - coord_rel.z;                                    // HLSL:162
    return coord_rel * size;                                            // HLSL:163
}

// HLSL:166-171
fn remap(val: f32, slope: f32) -> f32 {
    return 1.0 - exp(-slope * val);                                     // HLSL:167
}
fn tonemap(L: vec3<f32>, exposure: f32) -> vec3<f32> {
    return vec3<f32>(1.0, 1.0, 1.0) - exp(-exposure * L);               // HLSL:170
}

// HLSL:173-175  float trace_to_rho(float trace)
fn trace_to_rho(trace: f32) -> f32 {
    return cfg.sample_weight * (max(trace - cfg.trim_density, 0.0) + cfg.ambient_trace); // HLSL:174
}

// HLSL:177-180  float get_rho(float3 rp)
fn get_rho(rp: vec3<f32>) -> f32 {
    let trace = textureSampleLevel(
        tex_trace, tex_trace_sampler,
        rp / vec3<f32>(cfg.world_width, cfg.world_height, cfg.world_depth), 0.0).r;  // HLSL:178
    return trace_to_rho(trace);                                         // HLSL:179
}

// HLSL:182-185  float get_halo(float3 rp)
fn get_halo(rp: vec3<f32>) -> f32 {
    let halo = textureSampleLevel(
        tex_deposit, tex_deposit_sampler,
        rp / vec3<f32>(cfg.world_width, cfg.world_height, cfg.world_depth), 0.0).r;  // HLSL:183
    return 0.01 * cfg.galaxy_weight * halo;                             // HLSL:184
}

// HLSL:187-193  float3 get_halo_gradient(...) — reachable only via the
// disabled GRADIENT_GUIDING branch (call site HLSL:329, kept as a comment
// below); ported as a live function for 1:1 correspondence.
fn get_halo_gradient(rp: vec3<f32>, dp: f32) -> vec3<f32> {
    var gradient: vec3<f32>;
    gradient.x = get_halo(rp + vec3<f32>(dp, 0.0, 0.0)) - get_halo(rp - vec3<f32>(dp, 0.0, 0.0));  // HLSL:189
    gradient.y = get_halo(rp + vec3<f32>(0.0, dp, 0.0)) - get_halo(rp - vec3<f32>(0.0, dp, 0.0));  // HLSL:190
    gradient.z = get_halo(rp + vec3<f32>(0.0, 0.0, dp)) - get_halo(rp - vec3<f32>(0.0, 0.0, dp));  // HLSL:191
    return gradient / dp;                                                // HLSL:192
}

// HLSL:195-197  float3 get_emitted_trace_L(float rho)
fn get_emitted_trace_L(rho: f32) -> vec3<f32> {
    return 0.05 * textureSampleLevel(
        tex_palette_trace, tex_palette_trace_sampler,
        vec2<f32>(remap(rho, 1.0), 0.5), 0.0).rgb;                       // HLSL:196
}

// HLSL:199-201  float3 get_emitted_data_L(float rho)
fn get_emitted_data_L(rho: f32) -> vec3<f32> {
    return textureSampleLevel(
        tex_palette_data, tex_palette_data_sampler,
        vec2<f32>(remap(rho, 1.0), 0.5), 0.0).rgb;                       // HLSL:200
}

// QUIRK(sky_scalar_truncation): HLSL:203 declares get_sky_L as `float` but
// both #ifdef branches (HLSL:205,207) return `float3` — an implicit
// vec3->scalar truncation to `.x` at every call site (D3D11 accepts this
// silently). WHITESKY_ILLUMINATION is OFF in the shipping build (HLSL:17),
// so the live branch is float3(0,0,0), whose .x is 0.0 either way — port
// the truncated scalar return directly. Call sites re-broadcast to vec3
// explicitly (vec3<f32>(get_sky_L(rd))) where the HLSL's implicit
// float->float3 promotion back out would have applied.
fn get_sky_L(rd: vec3<f32>) -> f32 {
    // #ifdef WHITESKY_ILLUMINATION (disabled in default build) — kept for
    // 1:1 correspondence: HLSL:205 `return float3(sigma_e, sigma_e, sigma_e);`
    // (itself would truncate to `sigma_e` under this same quirk if enabled)
    return 0.0;                                                          // HLSL:207  float3(0.0, 0.0, 0.0) truncated to .x
}

// HLSL:211-213  float delta_step(float sigma_max_inv, float xi)
fn delta_step(sigma_max_inv: f32, xi: f32) -> f32 {
    return -log(max(xi, 0.001)) * sigma_max_inv;                        // HLSL:212
}

// HLSL:215-225  float delta_tracking(...) — do/while loop.
fn delta_tracking(rp: vec3<f32>, rd: vec3<f32>, t_min: f32, t_max: f32, rho_max_inv: f32, rng: ptr<function, RNG>) -> f32 {
    let sigma_max_inv = rho_max_inv / (cfg.sigma_a + cfg.sigma_s);      // HLSL:216
    var t = t_min;                                                      // HLSL:217
    var event_rho = 0.0;                                                // HLSL:218
    loop {
        t += delta_step(sigma_max_inv, rng_random_float(rng));          // HLSL:220
        event_rho = get_rho(rp + t * rd);                               // HLSL:221
        continuing {
            break if !(t <= t_max && rng_random_float(rng) > event_rho * rho_max_inv);  // HLSL:222
        }
    }
    return t;                                                            // HLSL:224
}

// HLSL:227-241  float occlusion_tracking(...) — dead in the shipping build
// (POINT_ILLUMINATION is off, HLSL:18); its only call site, HLSL:312, is
// inside the disabled #ifdef POINT_ILLUMINATION block in get_incident_L
// (kept below as a comment). Ported as a live but unreferenced function for
// 1:1 file correspondence. do/while -> loop { ...; continuing { break if
// !(cond); } }.
fn occlusion_tracking(rp: vec3<f32>, rd: vec3<f32>, t_min: f32, t_max: f32, rho_max_inv: f32, subsampling: f32, rng: ptr<function, RNG>) -> f32 {
    let sigma_max_inv = rho_max_inv / (cfg.sigma_a + cfg.sigma_s);      // HLSL:228
    var t = t_min;                                                      // HLSL:229
    var rho_sum = 0.0;                                                  // HLSL:230
    var iSteps = 0;                                                     // HLSL:231
    loop {
        t += subsampling * delta_step(sigma_max_inv, rng_random_float(rng));  // HLSL:233
        rho_sum += get_rho(rp + t * rd);                                // HLSL:234
        iSteps += 1;                                                    // HLSL:235
        continuing {
            break if !(t <= t_max);                                     // HLSL:236
        }
    }
    rho_sum /= f32(iSteps);                                             // HLSL:237
    let transmittance = exp(-(cfg.sigma_s + cfg.sigma_a) * rho_sum * (t_max - t_min));  // HLSL:238

    return transmittance;                                               // HLSL:240
}

// HLSL:243-248  void generate_basis(float3 dir, out float3 v1, out float3 v2)
fn generate_basis(dir: vec3<f32>, v1: ptr<function, vec3<f32>>, v2: ptr<function, vec3<f32>>) {
    let inv_norm = 1.0 / sqrt(dir.x * dir.x + dir.z * dir.z);           // HLSL:245
    (*v1) = vec3<f32>(dir.z * inv_norm, 0.0, -dir.x * inv_norm);        // HLSL:246
    (*v2) = cross(dir, (*v1));                                          // HLSL:247
}

// HLSL:250-268  float3 sample_HG(float3 v, float g, inout RNG rng)
fn sample_HG(v: vec3<f32>, g: f32, rng: ptr<function, RNG>) -> vec3<f32> {
    let xi = rng_random_float(rng);                                     // HLSL:252
    var cos_theta: f32;
    if (abs(g) > 1e-3) {                                                 // HLSL:254
        let sqr_term = (1.0 - g * g) / (1.0 - g + 2.0 * g * xi);        // HLSL:255
        cos_theta = (1.0 + g * g - sqr_term * sqr_term) / (2.0 * abs(g)); // HLSL:256
    } else {
        cos_theta = 1.0 - 2.0 * xi;                                     // HLSL:258
    }

    let sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));        // HLSL:261
    let phi = PI2 * rng_random_float(rng);                              // HLSL:262
    var v1: vec3<f32>;
    var v2: vec3<f32>;
    generate_basis(v, &v1, &v2);                                        // HLSL:264
    return sin_theta * cos(phi) * v1 +
           sin_theta * sin(phi) * v2 +
           cos_theta * v;                                                // HLSL:265-267
}

// HLSL:270-273  float pdf_HG(float g, float cos_angle)
fn pdf_HG(g: f32, cos_angle: f32) -> f32 {
    let denominator_cubicrt = sqrt(max(1.0 + g * g - 2.0 * g * cos_angle, NUMERICAL_EPSILON));  // HLSL:271
    return (1.0 - g * g) / (denominator_cubicrt * denominator_cubicrt * denominator_cubicrt);    // HLSL:272
}

// HLSL:275-345  float3 get_incident_L(...) — the full path-traced integrator.
fn get_incident_L(rp_in: vec3<f32>, rd_in: vec3<f32>, c_low: vec3<f32>, c_high: vec3<f32>, nBounces: i32, nRRStartOrder: i32, rng: ptr<function, RNG>) -> vec3<f32> {
    var rp = rp_in;
    var rd = rd_in;
    var L = vec3<f32>(0.0, 0.0, 0.0);                                   // HLSL:276
    var throughput = 1.0;                                                // HLSL:277
    let albedo = cfg.sigma_s / (cfg.sigma_a + cfg.sigma_s);             // HLSL:278
    let rho_max_inv = 1.0 / trace_to_rho(cfg.trace_max);                // HLSL:279

    // #ifdef POINT_ILLUMINATION (disabled in default build) — kept for 1:1
    // correspondence:
    // float3 trim_min = float3(trim_x_min, trim_y_min, trim_z_min);
    // float3 trim_max = float3(trim_x_max, trim_y_max, trim_z_max);
    // float3 l_rel_pos = 0.5 * (trim_min + trim_max);
    // float3 lp = c_low + l_rel_pos * c_high;

    for (var n = 0; n < nBounces; n++) {                                // HLSL:288

        // Sample collision distance
        let t = ray_AABB_intersection(rp, rd, c_low, c_high);           // HLSL:291
        let t_event = delta_tracking(rp, rd, 0.0, t.y, rho_max_inv, rng); // HLSL:292
        if (t_event >= t.y) {                                            // HLSL:293
            return L + vec3<f32>(throughput * get_sky_L(rd));           // HLSL:294  QUIRK(sky_scalar_truncation)
        }
        rp += t_event * rd;                                              // HLSL:295
        let rho_event = get_rho(rp);                                     // HLSL:296

        // Get emitted light
        var emission = vec3<f32>(0.0, 0.0, 0.0);                        // HLSL:299
        // #ifdef TRACE_ILLUMINATION (active in default build, HLSL:20):
        emission += get_emitted_trace_L(rho_event);                     // HLSL:301
        // #ifdef HALO_ILLUMINATION (active in default build, HLSL:19):
        let halo_event = get_halo(rp);                                  // HLSL:304
        emission += get_emitted_data_L(halo_event);                     // HLSL:305
        // #ifdef POINT_ILLUMINATION (disabled in default build) — kept for
        // 1:1 correspondence:
        // float3 ld = lp - rp;
        // float l_distance = length(ld);
        // ld = normalize(ld);
        // // Modified delta-tracking transmittance estimator
        // float transmittance = occlusion_tracking(rp, ld, 0.0, l_distance, rho_max_inv, 10.0, rng);
        // emission += 100.0 * galaxy_weight * transmittance / max(l_distance * l_distance, 1.0);
        L += throughput * rho_event * cfg.sigma_e * emission;           // HLSL:315

        // Adjust the path throughput (RR or modulate)
        // #ifdef RUSSIAN_ROULETTE (active in default build, HLSL:12):
        if (n >= nRRStartOrder && rng_random_float(rng) > albedo) {      // HLSL:319
            return L;                                                    // HLSL:320
        } else {
            throughput *= albedo;                                       // HLSL:322
        }

        // Sample new direction and continue the walk
        // #ifdef GRADIENT_GUIDING (disabled in default build) — kept for
        // 1:1 correspondence:
        // float3 grad = get_halo_gradient(rp, 1.0);
        // if (guiding_strength > INTENSITY_EPSILON && any(abs(grad) > float3(INTENSITY_EPSILON, INTENSITY_EPSILON, INTENSITY_EPSILON))) {
        //     float g = min(guiding_strength * length(grad), scattering_anisotropy);
        //     float3 grad_norm = normalize(grad);
        //     float3 rd_new = sample_HG(grad_norm, g, rng);
        //     float pdf = pdf_HG(g, dot(grad_norm, rd_new));
        //     throughput /= pdf;
        //     rd = rd_new;
        // } else {
        //     rd = uniform_unit_sphere(rng);
        // }
        rd = sample_HG(rd, cfg.scattering_anisotropy, rng);              // HLSL:341  (the #else branch, active)
    }
    return L;                                                             // HLSL:344
}

// ---------------------------------------------------------------------------
// Entry point (HLSL:347-455)
// ---------------------------------------------------------------------------

@compute @workgroup_size(10, 10, 1)
fn main(
    // HLSL:348  uint3 threadIDInGroup : SV_GroupThreadID
    @builtin(local_invocation_id) threadIDInGroup: vec3<u32>,
    // HLSL:348  uint3 groupID : SV_GroupID
    @builtin(workgroup_id) groupID: vec3<u32>,
    // HLSL:349  uint3 dispatchThreadId : SV_DispatchThreadID
    @builtin(global_invocation_id) dispatchThreadId: vec3<u32>,
) {
    let pixel_xy = dispatchThreadId.xy;                                  // HLSL:350

    // QUIRK(oob_dispatch_guard): NO HLSL counterpart. Host dispatch rounds UP
    // (ceil) to cover partial 10x10 tiles on resizable windows; D3D11 never
    // launched these threads. WGSL storage-buffer OOB indexing CLAMPS into a
    // real slot instead of discarding, so without this guard every tail-band
    // invocation would corrupt a real pixel's accumulator each frame
    // (translation-notes T2 class). Adjudicated 2026-08-12.
    if (pixel_xy.x >= u32(cfg.screen_width) || pixel_xy.y >= u32(cfg.screen_height)) {
        return;
    }

    // QUIRK(seed_idx_truncation): HLSL:351 assigns a uint3 expression
    // (threadIDInGroup + PT_GROUP_SIZE_X*PT_GROUP_SIZE_Y*groupID, itself a
    // uint3) to a scalar `uint idx` — an implicit truncation to `.x`.
    // PT_GROUP_SIZE_X * PT_GROUP_SIZE_Y == 100.
    let idx = threadIDInGroup.x + 100u * groupID.x;                      // HLSL:351

    // Row-major accumulator index (DESIGN §2.6), shared by the zero-fill
    // below and the read-modify-write at the end of this function.
    let pix_idx = pixel_xy.y * u32(cfg.screen_width) + pixel_xy.x;

    // If PT has been reset, zero-out the accumulation buffer
    if (cfg.pt_iteration == 0) {                                         // HLSL:354
        tex_accumulator[pix_idx] = vec4<f32>(0.0, 0.0, 0.0, 0.0);        // HLSL:355
    }

    // Initialize RNG with a unique seed for each iteration
    var rng: RNG;                                                         // HLSL:358
    rng_set_seed(&rng,
        wang_hash(1u + 73u * idx),                                        // HLSL:360
        wang_hash(1u + (pixel_xy.x + pixel_xy.y + pixel_xy.x * pixel_xy.y) * u32(cfg.pt_iteration + 1))  // HLSL:361
    );

    // Compute x and y ray directions in "neutral" camera position.
    let aspect_ratio = cfg.screen_width / cfg.screen_height;              // HLSL:365
    let rx = (f32(pixel_xy.x) + rng_random_float(&rng)) / cfg.screen_width * 2.0 - 1.0;   // HLSL:366
    var ry = (f32(pixel_xy.y) + rng_random_float(&rng)) / cfg.screen_height * 2.0 - 1.0;  // HLSL:367
    ry /= aspect_ratio;                                                    // HLSL:368

    // Initialize ray origin and direction
    const screen_distance: f32 = 4.5;                                     // HLSL:371
    const cam_offset_ratio: f32 = 0.45;                                   // HLSL:372
    let camera_pos = vec3<f32>(cfg.camera_x, cfg.camera_y, cfg.camera_z); // HLSL:373
    let camZ = normalize(-camera_pos);                                    // HLSL:374
    var camY = vec3<f32>(0.0, 0.0, 1.0);                                  // HLSL:375
    let camX = normalize(cross(camZ, camY));                              // HLSL:376
    camY = normalize(cross(camX, camZ));                                  // HLSL:377
    let screen_pos =
        camera_pos
        + (rx - cam_offset_ratio * cfg.camera_offset_x) * camX
        + (ry - cam_offset_ratio * cfg.camera_offset_y) * camY
        + screen_distance * camZ;                                         // HLSL:378-382

    // Get intersection of the ray with the volume AABB
    let grid_res = vec3<f32>(cfg.world_width, cfg.world_height, cfg.world_depth);  // HLSL:385
    let diagonal_AABB = vec3<f32>(1.0, cfg.world_height / cfg.world_width, cfg.world_depth / cfg.world_width);  // HLSL:386
    let c_low = -0.5 * diagonal_AABB;                                      // HLSL:387
    let c_high = 0.5 * diagonal_AABB;                                      // HLSL:388
    var rp = coord_normalized_to_texture(camera_pos, c_low, c_high, grid_res);  // HLSL:389
    var rd = normalize(coord_normalized_to_texture(screen_pos, c_low, c_high, grid_res) - rp);  // HLSL:390
    let c_low_trimmed = vec3<f32>(max(0.0, cfg.trim_x_min), max(0.0, cfg.trim_y_min), max(0.0, cfg.trim_z_min)) * grid_res;     // HLSL:391
    let c_high_trimmed = vec3<f32>(min(1.0, cfg.trim_x_max), min(1.0, cfg.trim_y_max), min(1.0, cfg.trim_z_max)) * grid_res;    // HLSL:392
    var t = ray_AABB_intersection(rp, rd, c_low_trimmed, c_high_trimmed);   // HLSL:393

    // Integrate...
    var path_L = vec3<f32>(0.0, 0.0, 0.0);                                 // HLSL:396
    if (t.y >= 0.0) {                                                       // HLSL:397
        t.x += RAY_EPSILON;                                                 // HLSL:398
        t.y -= RAY_EPSILON;                                                 // HLSL:399

        // Integrate along the ray segment that intersects the AABB
        rp = rp + t.x * rd;                                                 // HLSL:402
        rd = rd * (t.y - t.x);                                              // HLSL:403

        if (cfg.sigma_s < INTENSITY_EPSILON) {                              // HLSL:405
            // If there's no appreciable scattering, we can just use the
            // emission-absorption model and ray-march the solution
            let iSteps = i32(length(rd) / 1.71);                            // HLSL:408
            let dd = rd / f32(iSteps);                                      // HLSL:409
            rd = normalize(rd);                                             // HLSL:410
            var tau = 0.0;                                                  // HLSL:411
            rp += (rng_random_float(&rng) - 0.5) * dd;                      // HLSL:412
            var rho0 = get_rho(rp);                                         // HLSL:413
            var rho1: f32;                                                  // HLSL:413
            for (var i = 0; i < iSteps; i++) {                              // HLSL:414
                rp += dd;                                                    // HLSL:415
                rho1 = get_rho(rp);                                          // HLSL:416
                let rho = 0.5 * (rho0 + rho1);                              // HLSL:417

                tau += rho;                                                  // HLSL:419
                let transmittance = exp(-(cfg.sigma_a + cfg.sigma_s) * tau); // HLSL:420
                var emission = vec3<f32>(0.0, 0.0, 0.0);                    // HLSL:421
                // #ifdef TRACE_ILLUMINATION (active in default build, HLSL:20):
                emission += get_emitted_trace_L(rho);                       // HLSL:423
                // #ifdef HALO_ILLUMINATION (active in default build, HLSL:19):
                let halo_event = get_halo(rp);                              // HLSL:426
                emission += get_emitted_data_L(halo_event);                 // HLSL:427

                path_L += transmittance * rho * cfg.sigma_e * emission;     // HLSL:430
                rho0 = rho1;                                                 // HLSL:431
            }
        } else {
            // If there's significant scattering, we need the full path-traced solution
            rd = normalize(rd);                                             // HLSL:435
            path_L = get_incident_L(rp, rd, c_low_trimmed, c_high_trimmed, cfg.n_bounces + 1, 2, &rng);  // HLSL:436
            path_L *= PI2;                                                  // HLSL:437
        }
    } else {
        path_L = vec3<f32>(get_sky_L(rd));                                  // HLSL:440  QUIRK(sky_scalar_truncation)
    }

    // Accumulate LDR or HDR values?
    // HLSL:444  `path_L = (compressive_accumulation == 1) ? tonemap(path_L, exposure) : path_L;`
    // WGSL has no ternary operator; rewritten as an if-statement — purely
    // mechanical, zero semantic difference (same precedent as T7's
    // swizzle-compound-assignment rewrite in translation-notes.md).
    if (cfg.compressive_accumulation == 1) {                                // HLSL:444
        path_L = tonemap(path_L, cfg.exposure);
    }

    // Write out results for the current iteration
    // #ifdef TEMPORAL_ACCUMULATION (active in default build, HLSL:11):
    let current_value = tex_accumulator[pix_idx];                           // HLSL:448
    tex_accumulator[pix_idx] =
        current_value * f32(cfg.pt_iteration) / f32(cfg.pt_iteration + 1)
        + vec4<f32>(path_L, 1.0) / f32(cfg.pt_iteration + 1);               // HLSL:449-451
    // #else (disabled in default build) — kept for 1:1 correspondence:
    // tex_accumulator[pixel_xy] = float4(path_L, 1.0);
}
