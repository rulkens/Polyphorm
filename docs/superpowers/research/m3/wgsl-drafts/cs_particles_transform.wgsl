// ============================================================================
// cs_particles_transform.wgsl
// Draft port of shaders/cs_particles_transform.hlsl (Polyphorm, D3D11) to
// WGSL / Dawn.
//
// STATUS: DRAFT — not compiled. See translation-notes.md (M3, this directory).
//
// What this pass does: projects every particle (agent) from mold-world grid
// space into 2D screen space via view/projection, then splats a weighted
// count into an accumulation target keyed by screen pixel. HLSL:99,102 do
// this with InterlockedAdd on a RWTexture2D<uint> — an ATOMIC OP ON A
// STORAGE TEXTURE. WGSL has no atomic storage-texture type at all (only
// atomic<T> on storage/workgroup BUFFERS). See "THE HARD PROBLEM" below.
//
// FIDELITY CONTRACT: bug-for-bug port. Quirks are preserved, not fixed.
// Toggle mechanism (none needed here — no #ifdef in the source) would follow
// the M2 `override`-bool convention (docs/superpowers/research/m2/wgsl-drafts/
// translation-notes.md §1) if one is ever added.
// ============================================================================

// ---------------------------------------------------------------------------
// THE HARD PROBLEM: InterlockedAdd on RWTexture2D<uint> has no WGSL storage-
// texture equivalent.
//
// HLSL:1   RWTexture2D<uint> tex_out: register(u0);
// HLSL:99  InterlockedAdd(tex_out[out_pos], 10000);   // t < 0, galaxy_weight>0.001
// HLSL:102 InterlockedAdd(tex_out[out_pos], 10);      // t >= 0
//
// D3D11 guarantees InterlockedAdd on a typed UAV texel is a true atomic
// read-modify-write across the WHOLE dispatch (and, per the driver's UAV
// hazard tracking, safe even though millions of particles can map to the
// same output pixel — dense/zoomed-out views routinely have many-to-one
// splats). This is NOT the same risk profile as the racy deposit RMW in
// cs_agents_propagate (QUIRK, kept as a race, M2 notes §3 A3): that grid is
// large and 3D, so same-voxel collisions in one dispatch are rare. Here the
// grid is a 2D screen and the whole point of the pass is many-to-one
// accumulation — collisions are the COMMON case, not the tail.
//
// Two options were considered:
//
//   (a) CHOSEN — atomic<u32> STORAGE BUFFER, width*height elements,
//       row-major index (y * width + x), bound instead of a texture.
//       cs_particles_transform does atomicAdd() into it; cs_particles_blit
//       reads it back (plain load is safe there — see that file's header).
//       This is an EXACT behavioural match: true atomicity, order-
//       independent sum, identical final counts to D3D11.
//
//   (b) REJECTED — non-atomic texture_storage_2d<r32uint, read_write> with a
//       load-add-store RMW (the same shape as cs_agents_propagate's
//       preserved-race quirk). This would NOT be bug-for-bug: it introduces
//       a NEW race that does not exist upstream, and because texel
//       collisions are the common case here (not the rare case as in the 3D
//       deposit grid), the undercounting from lost updates would be
//       significant and visible — a genuinely different rendered image, not
//       a faithfully-preserved quirk. Preserving "the same kind of race" is
//       not the same as preserving "the same behaviour" when the collision
//       rate differs by orders of magnitude.
//
// (a) is the only option that is actually a port rather than a new bug, so
// it is the recommendation. Cost: a resource TYPE change (texture -> buffer)
// that main.cpp/graphics.h do not yet have a home for — see translation-
// notes.md "M3 implementation impact" for what that means for
// display_tex_uint, graphics::clear_texture_uint, and set_texture_compute.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) @binding(N) == HLSL
// register index N (compute convention, unchanged from M2).
// ---------------------------------------------------------------------------

// HLSL:7-32  cbuffer ConfigBuffer : register(b4)
//   Per carryover I3 (m2b-carryovers.md "M4 must handle"), b4-register
//   cbuffers move to @group(0) @binding(0) in this fork, same as every other
//   pass. The HLSL here declares only the FIRST 20 of RenderingConfig's 36
//   scalars (plus all 3 matrices) — main.cpp:263-312's full C++ layout is
//   declared below so ONE uniform buffer + ONE @group(0) bind group can be
//   shared with cs_particles_blit, the vs_2d/ps_particles_color pair, and
//   (M4) every volume/PT pass. Same "declare the whole shared struct" rule
//   as SimulationConfig in M2 notes §2.
//
// Layout verified field-by-field against main.cpp:263-312 and the
// static_assert at main.cpp:313 (sizeof == 3*64 + 36*4 == 336). All matrix
// members are 16-byte aligned by construction (they come first); every
// scalar after them is 4-byte aligned/sized with no member straddling a
// 16-byte boundary anywhere the C++ struct doesn't already group by fours,
// so WGSL's default (align-packed, no explicit @align/@size needed) struct
// layout reproduces the C++ offsets exactly. See translation-notes.md
// "RenderingConfig struct" for the full offset table and the matrix
// column-major cross-check (Matrix4x4 in cpplib/maths.h:278-291 stores
// columns contiguously and defines M*v as HLSL's mul(M,v) does — WGSL's
// mat4x4<f32> * vec4<f32> is the same convention, so `cfg.view * p` below is
// a literal, non-transposed translation of `mul(view_matrix, p)`).
struct RenderingConfig {
    projection: mat4x4<f32>,               // +0    HLSL:9
    view: mat4x4<f32>,                     // +64   HLSL:10
    model: mat4x4<f32>,                    // +128  HLSL:11

    texcoord_map: i32,                     // +192  HLSL:12  (unused here)
    trim_x_min: f32,                       // +196  HLSL:13
    trim_x_max: f32,                       // +200  HLSL:14
    trim_y_min: f32,                       // +204  HLSL:15

    trim_y_max: f32,                       // +208  HLSL:16
    trim_z_min: f32,                       // +212  HLSL:17
    trim_z_max: f32,                       // +216  HLSL:18
    trim_density: f32,                     // +220  HLSL:19  (unused here)

    world_width: f32,                      // +224  HLSL:20
    world_height: f32,                     // +228  HLSL:21
    world_depth: f32,                      // +232  HLSL:22
    screen_width: f32,                     // +236  HLSL:23

    screen_height: f32,                    // +240  HLSL:24
    sample_weight: f32,                    // +244  HLSL:25  (unused here — read by the blit)
    optical_thickness: f32,                // +248  HLSL:26  (unused here)
    highlight_density: f32,                // +252  HLSL:27  (unused here)

    galaxy_weight: f32,                    // +256  HLSL:28
    histogram_base: f32,                   // +260  HLSL:29  (unused here)
    overdensity_threshold_low: f32,        // +264  HLSL:30  (unused here)
    overdensity_threshold_high: f32,       // +268  HLSL:31  (unused here)

    // Members below +272 are NOT in the HLSL cbuffer (main.cpp's
    // RenderingConfig continues to pt_iteration / PT sigma terms / etc.).
    // Declared anyway so this struct is byte-identical to the full C++ type
    // and reusable verbatim by every other render/compute pass.
    camera_x: f32,                         // +272
    camera_y: f32,                         // +276
    camera_z: f32,                         // +280
    pt_iteration: i32,                     // +284

    sigma_s: f32,                          // +288
    sigma_a: f32,                          // +292
    sigma_e: f32,                          // +296
    trace_max: f32,                        // +300

    camera_offset_x: f32,                  // +304
    camera_offset_y: f32,                  // +308
    exposure: f32,                         // +312
    n_bounces: i32,                        // +316

    ambient_trace: f32,                    // +320
    compressive_accumulation: i32,         // +324
    guiding_strength: f32,                 // +328
    scattering_anisotropy: f32,            // +332
};
// sizeof == 336, matches main.cpp:313's static_assert exactly.
@group(0) @binding(0) var<uniform> cfg: RenderingConfig;

// HLSL:1  RWTexture2D<uint> tex_out: register(u0);
//   THE HARD PROBLEM (see block comment above): reinterpreted as an
//   atomic<u32> storage buffer, row-major, width = u32(cfg.screen_width),
//   height = u32(cfg.screen_height). Sizing this buffer to match
//   display_tex_uint's pixel count is an M3 host-side task (translation-
//   notes.md "M3 implementation impact") — the WGSL side only needs it
//   bound with size >= width*height*4 bytes.
@group(1) @binding(0) var<storage, read_write> tex_out: array<atomic<u32>>;

// HLSL:2  RWStructuredBuffer<float> particles_x: register(u2);
// HLSL:3  RWStructuredBuffer<float> particles_y: register(u3);
// HLSL:4  RWStructuredBuffer<float> particles_z: register(u4);
// HLSL:5  RWStructuredBuffer<float> particles_t: register(u6);
//   All four are declared RW in HLSL (RWStructuredBuffer) but this shader
//   only ever READS them, so — same narrowing precedent as cs_field_decay's
//   tex_in (M2 notes D1) — the WGSL access mode is narrowed to `read`. u1,
//   u5, u7 (trace tex, phi, weights) are not used by this shader and have no
//   binding here, matching main.cpp's original dispatch
//   (set_structured_buffer calls only for slots 6, 2, 3, 4 — see
//   translation-notes.md's main.cpp cross-reference).
//   NOTE: "particles_t" is the theta buffer (main.cpp's original dispatch
//   binds `particles_buffer_theta` at slot 6); the HLSL's local name `t`
//   inside main() is short for theta, not "type" or "time". Its sign is
//   used as a data-point/agent discriminator (see HLSL:97).
@group(1) @binding(2) var<storage, read> particles_x: array<f32>;
@group(1) @binding(3) var<storage, read> particles_y: array<f32>;
@group(1) @binding(4) var<storage, read> particles_z: array<f32>;
@group(1) @binding(6) var<storage, read> particles_t: array<f32>;

// ---------------------------------------------------------------------------
// HLSL:34-61 — wang_hash / random / random_sphere.
// DEAD CODE: none of these are called from main() in the HLSL (verified by
// inspection — main() only reads particles_x/y/z/t, no RNG calls anywhere
// in the body). Ported verbatim anyway for 1:1 file correspondence and in
// case a future edit resurrects them; costs nothing (Tint will DCE unused
// functions). Do not "clean these up" — that would silently diverge the
// file from the HLSL source it mirrors.
// ---------------------------------------------------------------------------

fn wang_hash(seed_in: u32) -> u32 {              // HLSL:34-42
    var seed = seed_in;
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed = seed * 9u;
    seed = seed ^ (seed >> 4u);
    seed = seed * 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

// HLSL:44-46  float random(int seed) — implicit int->uint conversion at the
// wang_hash(seed) call site. WGSL has no implicit numeric conversion;
// bitcast<u32> reproduces HLSL's bit-pattern-preserving implicit cast
// exactly (both int and uint are 32-bit two's-complement/unsigned words on
// this target). Dead code (see above), so this is unverified against a real
// compiler — flagged in translation-notes.md §7.
fn random_local(seed: i32) -> f32 {
    return f32(wang_hash(bitcast<u32>(seed)) % 1000u) / 1000.0;
}

fn random_sphere(hash: u32) -> vec3<f32> {        // HLSL:48-61
    let a = random_local(bitcast<i32>(hash));
    let b = random_local(bitcast<i32>(hash) + 3);
    let azimuth = a * 2.0 * 3.14159265;
    let polar = acos(2.0 * b - 1.0);

    var result: vec3<f32>;
    result.x = sin(polar) * cos(azimuth);
    result.y = cos(polar);
    result.z = sin(polar) * sin(azimuth);
    return result;
}

// ---------------------------------------------------------------------------
// Entry point
//
// HLSL:63  [numthreads(10, 10, 10)] == 1000 invocations — the IDENTICAL
// shape and the IDENTICAL over-default-limit situation as
// cs_agents_propagate.hlsl:86 (M2 notes §5). This fork ALREADY resolved that
// blocker for M2b by requesting the adapter's full
// maxComputeInvocationsPerWorkgroup at device creation, not the WebGPU
// default of 256 (cpplib/gpu/gpu_context.cpp:53,64-65: `required.
// maxComputeInvocationsPerWorkgroup = supported.maxComputeInvocationsPerWorkgroup`,
// fatal if < 256). cs_agents_propagate.wgsl runs with WG(10,10,10) in the
// shipped M2b build (headless smoke test passed), so WG(10,10,10) here is
// PROVEN safe on this device/build, not merely hoped-for. No reshape needed.
// ---------------------------------------------------------------------------
override WG_X: u32 = 10u;
override WG_Y: u32 = 10u;
override WG_Z: u32 = 10u;

@compute @workgroup_size(WG_X, WG_Y, WG_Z)
fn main(
    // HLSL:64-65  SV_GroupIndex -> local_invocation_index (identical
    // definition: tid.x + tid.y*sizeX + tid.z*sizeX*sizeY).
    // HLSL also takes SV_GroupThreadID and SV_DispatchThreadID as params but
    // never reads them in the body — dropped, same as cs_field_decay's D14.
    @builtin(local_invocation_index) thread_index: u32,
    @builtin(workgroup_id) group_id: vec3<u32>,
    @builtin(num_workgroups) n_groups: vec3<u32>,
) {
    // HLSL:66  uint idx = index + 1000 * (group_id.x + group_id.y*10 + group_id.z*100);
    //   Identical index-bijection technique as cs_agents_propagate.wgsl's
    //   entry point (M2 draft, lines ~275-297): written generically via
    //   num_workgroups so it reduces to the HLSL constants for the original
    //   dispatch shape (10, 10, grid_z) and stays a bijection under reshape.
    //   main.cpp's original dispatch (pre-M2b-stub):
    //     int32_t grid_z = (NUM_PARTICLES / 100) / THREAD_GROUP_SIZE;
    //     graphics::run_compute(10, 10, grid_z);
    //   THREAD_GROUP_SIZE == 1000 (== WG_X*WG_Y*WG_Z); same
    //   QUIRK(dispatch_truncation) tail-drop as the M2 agent shader applies
    //   here too (agents past grid_z*100000 are silently never dispatched).
    let group_idx = group_id.x
                  + group_id.y * n_groups.x
                  + group_id.z * n_groups.x * n_groups.y;
    let idx = thread_index + (WG_X * WG_Y * WG_Z) * group_idx;

    // HLSL:68-72
    let x = particles_x[idx];
    let y = particles_y[idx];
    let z = particles_z[idx];
    let t = particles_t[idx];
    let in_pos = vec3<f32>(x, y, z);

    // HLSL:74-76  Project point in "mold world texture space" to scene space.
    let world_size = vec3<f32>(cfg.world_width, cfg.world_height, cfg.world_depth);
    var in_posf = vec4<f32>(in_pos / world_size, 1.0);

    // HLSL:77-80  Trim-box early-out. Direct port: no barriers/shared memory
    // anywhere in this shader, so an early `return` here is unconditionally
    // safe (nothing downstream depends on cross-invocation state).
    if (in_posf.x < cfg.trim_x_min || in_posf.x > cfg.trim_x_max ||
        in_posf.y < cfg.trim_y_min || in_posf.y > cfg.trim_y_max ||
        in_posf.z < cfg.trim_z_min || in_posf.z > cfg.trim_z_max) {
        return;
    }

    // HLSL:81  in_posf.xyz = (2.0*in_posf.xyz - 1.0.xxx) * float3(1.0, h/w, d/w);
    //   `1.0.xxx` is HLSL's scalar-to-float3 broadcast literal; WGSL has no
    //   such literal syntax, so vec3<f32>(1.0) is substituted (identical
    //   value). Rewritten through a local `var` instead of a swizzle
    //   compound-assign — see translation-notes.md's uncertain-lines entry
    //   on why `in_posf.xyz *= ...` / `in_posf.yz *= ...` (HLSL:82) were
    //   NOT ported as WGSL swizzle-lvalue compound assignment.
    var p = in_posf.xyz;
    p = (2.0 * p - vec3<f32>(1.0)) *
        vec3<f32>(1.0, cfg.world_height / cfg.world_width, cfg.world_depth / cfg.world_width);
    // HLSL:82  in_posf.yz *= -1;  (D3D11/HLSL Y+Z axis flip vs texture space)
    p = vec3<f32>(p.x, -p.y, -p.z);
    in_posf = vec4<f32>(p, in_posf.w);

    // HLSL:85  float4 world_pos = mul(view_matrix, in_posf);
    //   HLSL's default cbuffer matrix packing is column-major (no
    //   `row_major` keyword used, and none of D3DCOMPILE_PACK_MATRIX_ROW_MAJOR
    //   is implied by this codebase); cpplib's Matrix4x4 (maths.h:278-291)
    //   stores columns contiguously and defines M*v with v as a column
    //   vector, matching WGSL's mat4x4<f32> * vec4<f32> exactly. No
    //   transpose is needed anywhere in this file.
    let world_pos = cfg.view * in_posf;

    // HLSL:88  float4 out_posf = mul(projection_matrix, world_pos);
    var out_posf = cfg.projection * world_pos;

    // HLSL:89  out_posf /= out_posf.w;
    out_posf = out_posf / out_posf.w;

    // HLSL:90  out_posf = out_posf * 0.5 + 0.5;
    //   Mixed vector-scalar arithmetic (vecN op scalar) IS a defined WGSL
    //   overload for +,-,*,/ (unlike implicit scalar->vector conversion on
    //   assignment, which WGSL disallows — see M2 notes A15). No explicit
    //   vec4<f32>(0.5) wrap is required, kept anyway for readability parity
    //   with the vec4<f32>(1.0) above.
    out_posf = out_posf * 0.5 + vec4<f32>(0.5);

    // HLSL:91-92
    if (out_posf.x < 0.0 || out_posf.y < 0.0 || out_posf.x > 1.0 || out_posf.y > 1.0) {
        return;
    }

    // HLSL:95  out_posf.xy *= float2(screen_width, screen_height);
    let screen_pos = out_posf.xy * vec2<f32>(cfg.screen_width, cfg.screen_height);
    // HLSL:96  uint2 out_pos = uint2(out_posf.xy);
    //   Float->uint truncation. Same conversion hazard class as M2 notes A6:
    //   HLSL is undefined for out-of-range values, WGSL is indeterminate.
    //   Both out_posf.x/.y are gated to [0.0, 1.0] just above, so
    //   screen_pos is gated to [0, screen_width] / [0, screen_height]
    //   INCLUSIVE of the upper bound (the boundary check above is `> 1.0`,
    //   not `>= 1.0`) — see the OOB-write guard immediately below for why
    //   that inclusive boundary matters here more than it did in M2.
    let out_pos = vec2<u32>(screen_pos);

    // ------------------------------------------------------------------
    // NEW REQUIRED GUARD — not present in the HLSL, and NOT optional.
    //
    // HLSL:96-103 write via a typed UAV (RWTexture2D<uint>). D3D11
    // guarantees an out-of-range typed-UAV texel WRITE is silently
    // DISCARDED (no-op) — this is exactly the OOB-store discard the M2b
    // carryovers call "test-pinned" for the r32float sim textures
    // (m2b-carryovers.md "Verified sound"). Because out_pos.x can legally
    // equal u32(cfg.screen_width) exactly (out_posf.x == 1.0 passes the
    // `> 1.0` rejection at HLSL:91), this pass DOES produce one-past-the-end
    // coordinates on every frame that has any particle exactly on the right
    // or bottom clip edge.
    //
    // WGSL/WebGPU's guarantee for an out-of-bounds dynamic array index into
    // a storage buffer is DIFFERENT: the spec requires the access be made
    // memory-safe via CLAMPING the index into range (or discarding — but
    // implementations are permitted to clamp), not "always a no-op". A
    // clamped OOB write on `tex_out` would land the atomicAdd on the LAST
    // element of the buffer instead of being dropped — i.e. it would
    // silently corrupt the bottom-right accumulation pixel with garbage
    // splats from every off-the-right/bottom-edge particle, every frame.
    // That is a new, buffer-specific analogue of the texture OOB-store
    // question the M2 notes flagged (§7 item 8) and it needs its own
    // explicit guard, unlike a texture store which the M2b carryovers
    // already proved discards safely.
    let width_px = u32(cfg.screen_width);
    let height_px = u32(cfg.screen_height);
    if (out_pos.x >= width_px || out_pos.y >= height_px) {
        return;
    }
    let out_idx = out_pos.y * width_px + out_pos.x;

    // HLSL:97-103
    //   t < 0.0 with galaxy_weight > 0.001 marks a data-point/galaxy splat
    //   (weight 10000, so the blit's `val < 10000` branch — see
    //   cs_particles_blit.wgsl — never fires for these, by construction);
    //   everything else (agents, or galaxies with galaxy_weight <= 0.001,
    //   which are then dropped entirely — QUIRK(galaxy_weight_gate), see
    //   translation-notes.md) is weight 10.
    //
    //   THE HARD PROBLEM, applied: atomicAdd replaces InterlockedAdd
    //   one-for-one. This IS the exact-fidelity translation (see file
    //   header) — order-independent atomic sum, same as D3D11.
    if (t < 0.0) {
        if (cfg.galaxy_weight > 0.001) {
            atomicAdd(&tex_out[out_idx], 10000u);         // HLSL:99
        }
        // else: QUIRK(galaxy_weight_gate) — a data point with t < 0 but
        // galaxy_weight <= 0.001 splats NOTHING (neither the 10000 nor the
        // 10 branch fires). Kept verbatim; this looks like it could be an
        // upstream oversight (falling through to the `10` branch might have
        // been intended) but "never fix upstream math" applies here too.
    } else {
        atomicAdd(&tex_out[out_idx], 10u);                 // HLSL:102
    }
}
