// ============================================================================
// cs_field_decay.wgsl
// Port of shaders/cs_field_decay.hlsl (Polyphorm, D3D11) to WGSL / Dawn.
//
// STATUS: DRAFT — not compiled. See translation-notes.md.
//
// What this pass does: one 3x3x3 weighted blur of the deposit field from the
// ping texture into the pong texture, multiplied by decay_factor; plus an
// in-place dithered decay of the trace field.
//
// FIDELITY CONTRACT + TOGGLE MECHANISM: identical to
// cs_agents_propagate.wgsl — pipeline-overridable `override` bools, branched
// with plain `if`, folded by Tint at pipeline creation. No preprocessor, no
// load-time string munging.
// ============================================================================

// ---------------------------------------------------------------------------
// Quirk toggles
// ---------------------------------------------------------------------------

// QUIRK(decay_weight_all_int3): kept for VAC parity
//   HLSL:69 reads
//       float weight = (all(int3(dx,dy,dz)) == 0) ? 1.0
//                    : 1.0 / sqrt(float(abs(dx)+abs(dy)+abs(dz)));
//   HLSL's `all(int3 v)` is "every component is non-zero". Comparing that
//   bool against 0 INVERTS it, so the flat 1.0 branch is taken whenever ANY
//   component is zero. Result: 19 of the 27 taps (centre + 6 faces + 12
//   edges) get weight 1.0 and only the 8 corners get 1/sqrt(3) = 0.5773503.
//       sum of weights = 19 + 8/sqrt(3) = 23.6188021
//   The comment above it ("Apply distance-based weighting to prevent
//   overestimation along diagonals") shows the author meant
//   `all(int3(dx,dy,dz) == 0)`, i.e. "is this the centre tap", which would
//   give centre 1.0, faces 1.0, edges 1/sqrt(2) = 0.7071068, corners
//   0.5773503, sum = 20.1043. The bug therefore over-weights the 12 EDGE
//   taps by ~41%, making the diffusion kernel measurably more diagonal —
//   exactly the artefact the comment says it was trying to avoid.
//   Setting this override to false selects the intended kernel; that is the
//   single highest-value A/B knob if M5 misses the correlation bar.
override QUIRK_DECAY_WEIGHT_ALL_INT3: bool = true;

// QUIRK(nonperiodic_low_boundary): kept for VAC parity
//   HLSL:71-73 wrap the tap coordinate with integer `%`. HLSL's `%` and
//   WGSL's `%` both take the sign of the DIVIDEND, so for p == 0 and
//   d == -1 the result is -1, not world_dim-1. The field is therefore
//   periodic on the HIGH side (world_dim % world_dim == 0) and absorbing on
//   the LOW side: the negative coordinate falls out of bounds and the load
//   returns 0. D3D11 typed-UAV OOB loads return 0.
//   CORRECTION (Task 4, see QUIRK(oob_load_zero_emulation) near load_oob_zero
//   below): WGSL does NOT guarantee a zero-returning OOB textureLoad — the
//   spec permits EITHER zero OR data from an arbitrary in-bounds texel, and
//   Task 4's GPU micro-tests measured this Dawn/Metal build actually
//   returning the latter (clamped to the far edge), which would have
//   silently broken this asymmetry. The bare-textureLoad "no emulation
//   needed" claim below was therefore WRONG; load_oob_zero() now performs
//   the emulation explicitly so the asymmetry described here actually holds.
//   Net effect: mass leaks out of the x=0 / y=0 / z=0 faces and re-enters at
//   the opposite faces. Also note the denominator `w` does NOT compensate —
//   it always sums 23.6188, so boundary voxels are additionally darkened.
override QUIRK_NONPERIODIC_LOW_BOUNDARY: bool = true;

// QUIRK(dithered_trace_decay): kept for VAC parity
//   HLSL:93 multiplies the trace by 0.985 + 0.01*rand instead of a constant,
//   "to avoid quantization errors of a constant decay factor" — a real
//   mitigation for f16 storage, where a fixed sub-ULP decay would round back
//   to the same half and never decay at all. We now store f32 (r32float), so
//   the mitigation is arguably unnecessary; it is nevertheless kept, because
//   it also changes the MEAN decay rate to 0.99 and adds spatially
//   correlated noise (see QUIRK(quantized_dither_seed) below) that the
//   published cube was fitted with.
override QUIRK_DITHERED_TRACE_DECAY: bool = true;

// QUIRK(rng_seed_guard_typo): kept for VAC parity — see the identical copy of
//   `struct RNG` in cs_agents_propagate.hlsl:34-70 / .wgsl. HLSL:29 here.
override QUIRK_RNG_SEED_GUARD_TYPO: bool = true;

// ---------------------------------------------------------------------------
// Bindings — @group(0) per-frame uniform, @group(1) @binding(N) == HLSL uN
// ---------------------------------------------------------------------------

// HLSL:5-17  cbuffer ConfigBuffer : register(b0)
//   The HLSL here declares only the FIRST TEN members of the C++
//   SimulationConfig (main.cpp:234) and marks eight of them "unused"; only
//   decay_factor and world_{width,height,depth} are read. We declare the full
//   64-byte struct so ONE uniform buffer and ONE @group(0) bind group can be
//   shared verbatim with cs_agents_propagate — safe because every member is a
//   4-byte scalar, so the offsets of the first ten are identical either way.
struct SimulationConfig {
    sense_spread:         f32,   // +0   unused here
    sense_distance:       f32,   // +4   unused here
    turn_angle:           f32,   // +8   unused here
    move_distance:        f32,   // +12  unused here
    deposit_value:        f32,   // +16  unused here
    decay_factor:         f32,   // +20  USED
    center_attraction:    f32,   // +24  unused here
    world_width:          i32,   // +28  USED
    world_height:         i32,   // +32  USED
    world_depth:          i32,   // +36  USED
    move_sense_coef:      f32,   // +40  not in the HLSL cbuffer
    normalization_factor: f32,   // +44  not in the HLSL cbuffer
    n_data_points:        i32,   // +48  not in the HLSL cbuffer
    n_agents:             i32,   // +52  not in the HLSL cbuffer
    n_iteration:          i32,   // +56  not in the HLSL cbuffer
    filler3:              i32,   // +60  not in the HLSL cbuffer
};
@group(0) @binding(0) var<uniform> cfg: SimulationConfig;

// HLSL:1  RWTexture3D<half2> tex_in : register(u0);
//   Declared RW in HLSL but only ever READ here, so the WebGPU access mode
//   can be `read` rather than `read_write`. That matters: it lets Dawn treat
//   the ping texture as read-only for this dispatch, and it documents the
//   ping-pong contract in the type system.
//   (Requires the `readonly_and_readwrite_storage_textures` WGSL language
//   feature — core in current WGSL / Dawn.)
@group(1) @binding(0) var tex_in:  texture_storage_3d<r32float, read>;

// HLSL:2  RWTexture3D<half2> tex_out : register(u1);   — write-only here.
@group(1) @binding(1) var tex_out: texture_storage_3d<r32float, write>;

// HLSL:3  RWTexture3D<half4> tex_trace : register(u2); — genuinely read_write
//   (HLSL:93 is a *=). Each invocation touches only its OWN texel, so unlike
//   the agent shader this read-modify-write is race-FREE.
@group(1) @binding(2) var tex_trace: texture_storage_3d<r32float, read_write>;

// QUIRK(r16f_channel_truncation): kept for VAC parity
//   main.cpp:568 creates the deposit ping-pong as DXGI_FORMAT_R16_FLOAT and
//   main.cpp:574 creates the trace as R16_FLOAT in the shipped REGIME_SDSS
//   build (VELOCITY_ANALYSIS / HALO_COLOR_ANALYSIS are commented out at
//   main.cpp:36/38), while this shader declares half2 / half4. D3D11 drops
//   the surplus components on store and returns 0 for them on load, so the
//   `.y` half of `v` below is always 0 and is always discarded. Single-channel
//   r32float is therefore an EXACT match, not a lossy one. Widening the format
//   is the toggle for this quirk; it is not expressible as an `override`.

// ---------------------------------------------------------------------------
// RNG — byte-identical copy of HLSL:19-55, which is itself a verbatim copy of
// cs_agents_propagate.hlsl:34-70. Same constants, same order, same typo.
// ---------------------------------------------------------------------------

const BAD_W: u32 = 0x464fffffu;   // HLSL:20
const BAD_Z: u32 = 0x9068ffffu;   // HLSL:21

struct RNG {
    m_w: u32,   // HLSL:22
    m_z: u32,   // HLSL:23
};

fn wang_hash(seed_in: u32) -> u32 {          // HLSL:47-54
    var seed = seed_in;
    seed = (seed ^ 61u) ^ (seed >> 16u);     // HLSL:48
    seed = seed * 9u;                        // HLSL:49
    seed = seed ^ (seed >> 4u);              // HLSL:50
    seed = seed * 0x27d4eb2du;               // HLSL:51
    seed = seed ^ (seed >> 15u);             // HLSL:52
    return seed;                             // HLSL:53
}

fn set_seed(rng: ptr<function, RNG>, seed1: u32, seed2: u32) {   // HLSL:25-30
    (*rng).m_w = seed1;                                          // HLSL:26
    (*rng).m_z = seed2;                                          // HLSL:27
    if ((*rng).m_w == 0u || (*rng).m_w == BAD_W) {               // HLSL:28
        (*rng).m_w = (*rng).m_w + 1u;
    }
    if (QUIRK_RNG_SEED_GUARD_TYPO) {
        // QUIRK(rng_seed_guard_typo): kept for VAC parity  <- HLSL:29 verbatim
        // (tests m_w where it plainly means m_z; the m_w clause is dead
        //  because line 28 already guaranteed m_w != 0)
        if ((*rng).m_w == 0u || (*rng).m_z == BAD_Z) {
            (*rng).m_z = (*rng).m_z + 1u;
        }
    } else {
        if ((*rng).m_z == 0u || (*rng).m_z == BAD_Z) {
            (*rng).m_z = (*rng).m_z + 1u;
        }
    }
}

fn random_uint(rng: ptr<function, RNG>) -> u32 {                        // HLSL:37-41
    (*rng).m_z = 36969u * ((*rng).m_z & 65535u) + ((*rng).m_z >> 16u);  // HLSL:38
    (*rng).m_w = 18000u * ((*rng).m_w & 65535u) + ((*rng).m_w >> 16u);  // HLSL:39
    return ((*rng).m_z << 16u) + (*rng).m_w;                            // HLSL:40
}

fn random_float(rng: ptr<function, RNG>) -> f32 {                       // HLSL:43-45
    return f32(random_uint(rng)) / f32(0xFFFFFFFFu);                    // HLSL:44
}

// ---------------------------------------------------------------------------
// Entry point
//
// HLSL:57  [numthreads(8,8,8)]  == 512 invocations, over WebGPU's DEFAULT
// maxComputeInvocationsPerWorkgroup of 256. Unlike the agent shader, this
// pass has no shared memory, no barriers, and every invocation writes exactly
// one texel derived only from its own global id — so reshaping the workgroup
// (e.g. 8x8x4 with double the Z dispatch) is provably BITWISE identical, not
// merely statistically. Kept as 8x8x8 by default via overrides; the host
// flips WG_Z to 4 and doubles dispatch.z if the adapter refuses 512.
// Host dispatch (main.cpp:1038): (GRID_X/8, GRID_Y/8, GRID_Z/8) — the grid
// dims are rounded to multiples of 8 at main.cpp:412-414, so coverage is
// exact with no tail guard needed (and the original has none).
// ---------------------------------------------------------------------------
override WG_X: u32 = 8u;
override WG_Y: u32 = 8u;
override WG_Z: u32 = 8u;

// QUIRK(oob_load_zero_emulation): D3D11 guarantees OOB typed-UAV loads return 0;
// WGSL only PERMITS zero and this Dawn/Metal build clamps to edge (measured, see
// .superpowers task-4 report / m2a-carryovers). Explicit guard restores D3D11 semantics.
//   The WGSL spec (textureLoad, "Out-of-bounds" clause) allows an
//   out-of-range logical texel address to return EITHER the zero value OR
//   "the data for some texel within bounds of the texture" — implementation's
//   choice. Task 4's GPU micro-tests measured this Dawn build's actual choice
//   to be clamp-to-edge, not zero-fill, which silently breaks the
//   QUIRK(nonperiodic_low_boundary) parity below (the low-boundary darkening
//   that translation-notes.md §7.9 derives, and that the published SDSS VAC
//   cube was fitted against, depends on OOB loads reading 0). This helper
//   makes the zero-fill explicit instead of relying on unspecified hardware
//   behaviour. Only tex_in's .x channel is ever consumed by callers, so the
//   unused .yzw lanes of the zero-fill vector are inert; (0,0,0,0) vs
//   (0,0,0,1) — the alpha convention D3D11 typed-UAV zero-fills use — is
//   unobservable here either way, so plain vec4<f32>(0.0) is used.
fn load_oob_zero(coord: vec3<i32>) -> vec4<f32> {
    if (all(coord >= vec3<i32>(0)) &&
        coord.x < cfg.world_width && coord.y < cfg.world_height && coord.z < cfg.world_depth) {
        return textureLoad(tex_in, coord);
    }
    return vec4<f32>(0.0, 0.0, 0.0, 0.0);
}

@compute @workgroup_size(WG_X, WG_Y, WG_Z)
fn main(
    // HLSL:58-59 also declare SV_GroupThreadID and SV_GroupID; both are unused
    // in the body, so only the dispatch id is carried over.
    @builtin(global_invocation_id) dispatchThreadId: vec3<u32>,
) {
    let p: vec3<u32> = dispatchThreadId.xyz;                            // HLSL:60

    // -----------------------------------------------------------------------
    // HLSL:62-80  3x3x3 weighted average of the deposit neighbourhood
    // -----------------------------------------------------------------------
    var v = vec2<f32>(0.0, 0.0);                                        // HLSL:64
    var w: f32 = 0.0;                                                   // HLSL:65
    for (var dx: i32 = -1; dx <= 1; dx = dx + 1) {                      // HLSL:66
        for (var dy: i32 = -1; dy <= 1; dy = dy + 1) {                  // HLSL:67
            for (var dz: i32 = -1; dz <= 1; dz = dz + 1) {              // HLSL:68

                // HLSL:69 — see QUIRK(decay_weight_all_int3) at the top.
                // WGSL's all() takes vecN<bool>, so HLSL's all(int3) becomes
                // an explicit "every component non-zero" test.
                // `falloff` is deliberately NOT hoisted out of the branches:
                // at the centre tap it is 1.0/sqrt(0.0), which is an
                // indeterminate value in WGSL (and inf in HLSL). Both forms
                // below take the 1.0 branch there, so it is never evaluated —
                // which is exactly why the buggy kernel never produced NaNs.
                var weight: f32;
                let hlsl_all_nonzero = all(vec3<bool>(dx != 0, dy != 0, dz != 0));
                if (QUIRK_DECAY_WEIGHT_ALL_INT3) {
                    // QUIRK(decay_weight_all_int3): kept for VAC parity
                    // 19 taps @ 1.0, 8 corners @ 0.5773503, sum w = 23.6188021
                    if (!hlsl_all_nonzero) {
                        weight = 1.0;
                    } else {
                        weight = 1.0 / sqrt(f32(abs(dx) + abs(dy) + abs(dz)));
                    }
                } else {
                    // Intended kernel: centre @ 1.0, everything else falls off.
                    // centre 1.0 / faces 1.0 / edges 0.7071068 / corners
                    // 0.5773503, sum w = 20.1043.
                    if (dx == 0 && dy == 0 && dz == 0) {
                        weight = 1.0;
                    } else {
                        weight = 1.0 / sqrt(f32(abs(dx) + abs(dy) + abs(dz)));
                    }
                }

                var txcoord = vec3<i32>(p) + vec3<i32>(dx, dy, dz);     // HLSL:70
                if (QUIRK_NONPERIODIC_LOW_BOUNDARY) {
                    // QUIRK(nonperiodic_low_boundary): kept for VAC parity
                    // WGSL `%` on i32 is remainder-with-sign-of-dividend,
                    // identical to HLSL, so this is a literal port. -1 stays
                    // -1 and the load below falls out of bounds -> 0.
                    txcoord.x = txcoord.x % cfg.world_width;            // HLSL:71
                    txcoord.y = txcoord.y % cfg.world_height;           // HLSL:72
                    txcoord.z = txcoord.z % cfg.world_depth;            // HLSL:73
                } else {
                    // Truly periodic wrap (Euclidean mod).
                    txcoord.x = (txcoord.x % cfg.world_width  + cfg.world_width)  % cfg.world_width;
                    txcoord.y = (txcoord.y % cfg.world_height + cfg.world_height) % cfg.world_height;
                    txcoord.z = (txcoord.z % cfg.world_depth  + cfg.world_depth)  % cfg.world_depth;
                }

                // HLSL:74  float2 val = tex_in[txcoord];
                //   r32float load yields (r, 0, 0, 1); .xy == (r, 0) matches
                //   the D3D11 half2-on-R16_FLOAT result exactly.
                //   Out-of-bounds (negative txcoord) -> zero vector, matching
                //   D3D11 typed-UAV OOB load behaviour. QUIRK(oob_load_zero_emulation):
                //   explicit guard (see load_oob_zero above) — WGSL/this Dawn build
                //   does NOT do this for us on a bare textureLoad.
                let val = load_oob_zero(txcoord).xy;                    // HLSL:74
                v = v + weight * val;                                   // HLSL:75
                w = w + weight;                                         // HLSL:76
            }
        }
    }
    v = v / w;                                                          // HLSL:80

    // HLSL:82-84  Decay the deposit by a constant factor
    v = v * cfg.decay_factor;                                           // HLSL:83
    // v.y is structurally always 0 here (see QUIRK(r16f_channel_truncation))
    // and is discarded by the single-channel format; retained so the
    // HALO_COLOR_ANALYSIS regime is a format change only.
    textureStore(tex_out, p, vec4<f32>(v, 0.0, 0.0));                   // HLSL:84

    // -----------------------------------------------------------------------
    // HLSL:86-93  Decay the trace a little
    // HLSL:87 keeps a plain `tex_trace[p] *= 0.99;` commented out — that is
    // the mean of the dithered version below, and the natural "quirk off"
    // value, which is why the else-branch uses 0.99 rather than 1.0.
    // -----------------------------------------------------------------------
    var rng: RNG;
    // QUIRK(quantized_dither_seed): kept for VAC parity
    //   HLSL:90  wang_hash(uint(113.0*v.x)) — a f32->u32 TRUNCATION of the
    //   post-decay deposit. For the overwhelming majority of voxels
    //   113*v.x < 1, so seed1 collapses to wang_hash(0) across all of empty
    //   space; the dither is therefore far from white noise and is spatially
    //   banded by deposit magnitude.
    //   HLSL:91  wang_hash(uint(p.x*p.y*p.z)) — u32 product with wraparound
    //   (defined identically in HLSL and WGSL). It is ZERO for every voxel on
    //   the x=0, y=0 or z=0 planes, so those three faces share one seed pair
    //   per deposit band; and the product aliases heavily elsewhere
    //   (p=(2,3,4) and p=(1,4,6) collide). Preserved verbatim.
    set_seed(&rng,
        wang_hash(u32(113.0 * v.x)),                                    // HLSL:90
        wang_hash(p.x * p.y * p.z));                                    // HLSL:91

    var trace_decay: f32;
    if (QUIRK_DITHERED_TRACE_DECAY) {
        // QUIRK(dithered_trace_decay): kept for VAC parity
        trace_decay = 0.985 + 0.01 * random_float(&rng);                // HLSL:93
    } else {
        trace_decay = 0.99;                                             // HLSL:87 (commented-out original)
    }
    // Race-free RMW: p is this invocation's own texel and nothing else
    // touches it in this dispatch.
    let trace_prev = textureLoad(tex_trace, p);
    textureStore(tex_trace, p, trace_prev * trace_decay);               // HLSL:93
}
