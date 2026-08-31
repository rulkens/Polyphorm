// ============================================================================
// cs_agents_propagate.wgsl
// Port of shaders/cs_agents_propagate.hlsl (Polyphorm, D3D11) to WGSL / Dawn.
//
// STATUS: DRAFT — not compiled. See translation-notes.md for the full
// HLSL-line -> WGSL-line difference table, the SimulationConfig byte-offset
// table, and the "uncertain lines" list.
//
// FIDELITY CONTRACT
//   Bug-for-bug faithful to the D3D11 original until the M5 VAC validation
//   gate passes. Every deliberate deviation-from-clean-code carries a
//   `// QUIRK(<name>): kept for VAC parity` marker.
//
// QUIRK TOGGLE MECHANISM
//   WGSL has no preprocessor. We use **pipeline-overridable constants**
//   (`override`) of type bool, branched on with plain `if`. Tint folds the
//   dead branch at pipeline-creation time, so this costs nothing at runtime
//   and — unlike load-time string preprocessing — keeps the on-disk shader
//   a single, editable, always-valid WGSL file (preserving the HLSL
//   edit-and-reload iteration loop the fork wants to keep).
//   Host side: WGPUComputePipelineDescriptor -> constants[] keyed by the
//   override *name* (Dawn supports name keys when no @id is given).
//   This gives M5's "quirk-by-quirk A/B hunt" a runtime bisection knob
//   without recompiling the C++.
//
// RESOURCE-LEVEL NOTE (not runtime-toggleable, see notes §Channel truncation)
//   In the shipped REGIME_SDSS configuration (VELOCITY_ANALYSIS and
//   HALO_COLOR_ANALYSIS both commented out in main.cpp:36/38), main.cpp:568
//   and main.cpp:574 create BOTH the deposit ping-pong textures and the
//   trace texture as DXGI_FORMAT_R16_FLOAT — ONE channel — while this shader
//   declares RWTexture3D<half2> / RWTexture3D<half4>. D3D11 typed-UAV
//   semantics silently drop the extra components on store and return
//   (r, 0, 0, 1) on load. So the deposit "color" channel and the trace
//   direction channels are DEAD in the regime we are porting. Mapping both
//   to a single-channel `r32float` read_write storage texture is therefore
//   an exact behavioural match, not an approximation. The vec2/vec4
//   arithmetic below is retained verbatim so the VELOCITY/HALO_COLOR
//   regimes can be re-enabled later by widening the format only.
// ============================================================================

// ---------------------------------------------------------------------------
// Quirk / feature toggles (were `#define`s or latent bugs in the HLSL)
// ---------------------------------------------------------------------------

// HLSL:11  #define PROBABILISTIC_SAMPLING            (ON in original)
override PROBABILISTIC_SAMPLING: bool = true;
// HLSL:12  #define AGENT_REROUTING                   (ON in original)
override AGENT_REROUTING: bool = true;
// HLSL:13  // #define FIXED_AGENT_DISTANCE_SAMPLING   (OFF in original)
override FIXED_AGENT_DISTANCE_SAMPLING: bool = false;
// HLSL:14  // #define IGNORE_DATA                     (OFF in original)
override IGNORE_DATA: bool = false;

// QUIRK(rng_seed_guard_typo): kept for VAC parity
//   HLSL:44 reads `if (m_w == 0U || m_z == BAD_Z) ++m_z;` — the first clause
//   tests m_w, almost certainly a copy-paste of line 43 where it should test
//   m_z. Consequence: a zero m_z seed is never repaired (m_w is already
//   non-zero by line 43, and ++BAD_W == 0x46500000 != 0, so the m_w clause on
//   line 44 is dead). Preserved because it perturbs the RNG stream for every
//   agent whose wang_hash(uint(x*y*z)) happens to be 0 — notably every agent
//   sitting on a plane x==0, y==0 or z==0.
override QUIRK_RNG_SEED_GUARD_TYPO: bool = true;

// QUIRK(int3_truncated_sensing): kept for VAC parity
//   HLSL:140/145 truncate the *float* sensing offset with int3(...) before
//   adding it to the integer agent cell. Truncation is toward zero, so the
//   sensed cell is systematically biased toward the agent along every axis
//   (a -0.9 offset senses the agent's own cell). vec3<i32>() has identical
//   toward-zero semantics, so the port is literal.
override QUIRK_INT3_TRUNCATED_SENSING: bool = true;

// QUIRK(dead_current_deposit_read): kept for VAC parity
//   HLSL:138 loads `current_deposit` and never uses it — HLSL:230 overwrites
//   it before its only read at HLSL:231. The load is dead but it IS a real
//   memory transaction against a texture that other invocations are
//   concurrently writing; keeping it preserves the (unobservable, but free)
//   original. Turning this off is a pure optimisation with no output change.
override QUIRK_DEAD_CURRENT_DEPOSIT_READ: bool = true;

// ---------------------------------------------------------------------------
// Bindings
//
// Proposed layout (see notes §Bind group layout proposal):
//   @group(0) = per-frame uniform, one bind group shared by every pass
//   @group(1) = per-pass resources, @binding(N) == the HLSL register index N,
//               so u0..u7 map 1:1 and the diff against the HLSL stays readable
// ---------------------------------------------------------------------------

// HLSL:16-32  cbuffer ConfigBuffer : register(b0)
// Mirrors main.cpp:234 `struct SimulationConfig` in full (16 x 4B = 64B).
// The HLSL cbuffer only declared the first 14 members; declaring all 16 here
// documents the C++ layout and makes the WGSL struct size (64) match
// sizeof(SimulationConfig) exactly. All members are 4-byte scalars, so WGSL's
// uniform-address-space rules insert NO padding — see notes for the offsets.
struct SimulationConfig {
    sense_spread:         f32,   // +0
    sense_distance:       f32,   // +4
    turn_angle:           f32,   // +8
    move_distance:        f32,   // +12
    deposit_value:        f32,   // +16
    decay_factor:         f32,   // +20
    center_attraction:    f32,   // +24
    world_width:          i32,   // +28
    world_height:         i32,   // +32
    world_depth:          i32,   // +36
    move_sense_coef:      f32,   // +40
    normalization_factor: f32,   // +44
    n_data_points:        i32,   // +48
    n_agents:             i32,   // +52
    n_iteration:          i32,   // +56  (not declared in the HLSL cbuffer)
    filler3:              i32,   // +60  (not declared in the HLSL cbuffer)
};
@group(0) @binding(0) var<uniform> cfg: SimulationConfig;

// HLSL:1  RWTexture3D<half2> tex_deposit : register(u0);
//   half2 -> r32float (single channel). Read AND written by this shader, and
//   written non-atomically by concurrent invocations, so it MUST be
//   `read_write` — which in WebGPU restricts us to r32uint/r32sint/r32float.
// QUIRK(nonatomic_deposit_accumulation): kept for VAC parity
//   HLSL:111 and HLSL:255 do a load-add-store with no atomic. This is the
//   original's deliberate trade (no float atomics on GPUs; the lost updates
//   launder into MCPM's Monte Carlo noise). WGSL has no float atomics either,
//   so the racy read-modify-write is a faithful port, not a regression.
//   NOT toggleable: there is no atomic-float alternative to A/B against.
@group(1) @binding(0) var tex_deposit: texture_storage_3d<r32float, read_write>;

// HLSL:2  RWTexture3D<half4> tex_trace : register(u1);
// QUIRK(nonatomic_trace_accumulation): kept for VAC parity  (HLSL:258)
@group(1) @binding(1) var tex_trace: texture_storage_3d<r32float, read_write>;

// HLSL:4-9  RWStructuredBuffer<float> particles_* : register(u2..u7)
@group(1) @binding(2) var<storage, read_write> particles_x:       array<f32>;
@group(1) @binding(3) var<storage, read_write> particles_y:       array<f32>;
@group(1) @binding(4) var<storage, read_write> particles_z:       array<f32>;
@group(1) @binding(5) var<storage, read_write> particles_phi:     array<f32>;
@group(1) @binding(6) var<storage, read_write> particles_theta:   array<f32>;
@group(1) @binding(7) var<storage, read_write> particles_weights: array<f32>;

// ---------------------------------------------------------------------------
// RNG  (HLSL:34-70 `struct RNG` with member functions)
// WGSL structs have no methods; these become free functions taking
// `ptr<function, RNG>`. Constants and evaluation order are byte-identical.
// ---------------------------------------------------------------------------

const BAD_W: u32 = 0x464fffffu;   // HLSL:20 #define BAD_W
const BAD_Z: u32 = 0x9068ffffu;   // HLSL:21 #define BAD_Z

struct RNG {
    m_w: u32,   // HLSL:37
    m_z: u32,   // HLSL:38
};

// HLSL:62-69  uint wang_hash(uint seed)
// Stateless in the original too (reads no members), so a free function is exact.
fn wang_hash(seed_in: u32) -> u32 {
    var seed = seed_in;
    seed = (seed ^ 61u) ^ (seed >> 16u);   // HLSL:63
    seed = seed * 9u;                      // HLSL:64
    seed = seed ^ (seed >> 4u);            // HLSL:65
    seed = seed * 0x27d4eb2du;             // HLSL:66
    seed = seed ^ (seed >> 15u);           // HLSL:67
    return seed;                           // HLSL:68
}

// HLSL:40-45  void set_seed(uint seed1, uint seed2)
fn set_seed(rng: ptr<function, RNG>, seed1: u32, seed2: u32) {
    (*rng).m_w = seed1;                                            // HLSL:41
    (*rng).m_z = seed2;                                            // HLSL:42
    if ((*rng).m_w == 0u || (*rng).m_w == BAD_W) {                 // HLSL:43
        (*rng).m_w = (*rng).m_w + 1u;
    }
    if (QUIRK_RNG_SEED_GUARD_TYPO) {
        // QUIRK(rng_seed_guard_typo): kept for VAC parity   <- HLSL:44 verbatim
        if ((*rng).m_w == 0u || (*rng).m_z == BAD_Z) {
            (*rng).m_z = (*rng).m_z + 1u;
        }
    } else {
        // The evidently intended guard.
        if ((*rng).m_z == 0u || (*rng).m_z == BAD_Z) {
            (*rng).m_z = (*rng).m_z + 1u;
        }
    }
}

// HLSL:52-56  uint random_uint()
fn random_uint(rng: ptr<function, RNG>) -> u32 {
    (*rng).m_z = 36969u * ((*rng).m_z & 65535u) + ((*rng).m_z >> 16u);  // HLSL:53
    (*rng).m_w = 18000u * ((*rng).m_w & 65535u) + ((*rng).m_w >> 16u);  // HLSL:54
    return ((*rng).m_z << 16u) + (*rng).m_w;                            // HLSL:55
}

// HLSL:58-60  float random_float()
// f32(0xFFFFFFFFu) rounds to 4294967296.0 in both HLSL and WGSL, so the
// divisor — and therefore the [0,1) mapping — is bit-identical.
fn random_float(rng: ptr<function, RNG>) -> f32 {
    return f32(random_uint(rng)) / f32(0xFFFFFFFFu);                    // HLSL:59
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// HLSL:72-75  float3 rotate(float3 v, float3 a, float angle)  (Rodrigues)
fn rotate(v: vec3<f32>, a: vec3<f32>, angle: f32) -> vec3<f32> {
    return cos(angle) * v
         + sin(angle) * cross(a, v)
         + dot(a, v) * (1.0 - cos(angle)) * a;                          // HLSL:73
}

// HLSL:77-79  float mod(float x, float y)
// RENAMED: `mod` is a WGSL *reserved word*. Semantics unchanged.
// NOTE the deliberate asymmetry with cs_field_decay: agent *movement* wraps
// with this FLOOR-mod (always non-negative, truly periodic), while the decay
// blur wraps with integer `%` (sign-of-dividend, NOT periodic on the low
// side). Both are preserved.
fn mod_floor(x: f32, y: f32) -> f32 {
    return x - y * floor(x / y);                                        // HLSL:78
}

// QUIRK(oob_load_zero_emulation): D3D11 guarantees OOB typed-UAV loads return 0;
// WGSL only PERMITS zero and this Dawn/Metal build clamps to edge (measured, see
// .superpowers task-4 report / m2a-carryovers). Explicit guard restores D3D11 semantics.
//   Companion to the identical helper in cs_field_decay.wgsl, split in two
//   here because tex_deposit and tex_trace are different bindings. Guards
//   every LOAD whose coordinate can leave the grid: the sensing rays
//   (QUIRK(nonperiodic_sensing) is explicitly non-wrapping, so an agent
//   sensing near an edge routinely constructs an out-of-range coordinate),
//   the dead current_deposit read, and the post-move reads whose coordinate
//   comes from mod_floor (normally in-range, but HLSL:220-223's own comment
//   flags the residual f32 rounding hazard that can land it exactly on the
//   grid dimension). Store call sites are UNCHANGED — Task 4 measured OOB
//   textureStore as correctly discarded on this build/spec (a hard "will not
//   be executed" guarantee, unlike textureLoad's OOB fallback), so no
//   emulation is needed there. Only .x (deposit)/.x (trace, single-channel
//   r32float — see QUIRK(r16f_channel_truncation)) is ever consumed by
//   callers, so the zero-fill's unused lanes are inert; plain vec4<f32>(0.0)
//   is used (see cs_field_decay.wgsl's load_oob_zero for the same note).
fn load_deposit_oob_zero(coord: vec3<i32>) -> vec4<f32> {
    if (all(coord >= vec3<i32>(0)) &&
        coord.x < cfg.world_width && coord.y < cfg.world_height && coord.z < cfg.world_depth) {
        return textureLoad(tex_deposit, coord);
    }
    return vec4<f32>(0.0, 0.0, 0.0, 0.0);
}

fn load_trace_oob_zero(coord: vec3<i32>) -> vec4<f32> {
    if (all(coord >= vec3<i32>(0)) &&
        coord.x < cfg.world_width && coord.y < cfg.world_height && coord.z < cfg.world_depth) {
        return textureLoad(tex_trace, coord);
    }
    return vec4<f32>(0.0, 0.0, 0.0, 0.0);
}

// HLSL:81-84
const DIR_SAMPLE_POINTS: i32 = 8;      // unused (only the dead naive sampler used it)
const PI: f32     = 3.141592;          // NOTE: the original's truncated PI, kept verbatim
const HALFPI: f32 = 0.5 * PI;
const TWOPI: f32  = 2.0 * PI;

// ---------------------------------------------------------------------------
// Entry point
//
// HLSL:86  [numthreads(10,10,10)]  == 1000 invocations per group.
// WebGPU's DEFAULT maxComputeInvocationsPerWorkgroup is 256, so 1000 is
// over the default limit and must either be requested at device creation
// (Metal/Apple Silicon reports 1024; query the adapter) or the shape must be
// reshaped. The index math below is written shape-independently so a reshape
// is provably safe — see notes §numthreads mapping. Overriding the workgroup
// size from the host keeps that switch a one-line pipeline-constant change.
// ---------------------------------------------------------------------------
override WG_X: u32 = 10u;
override WG_Y: u32 = 10u;
override WG_Z: u32 = 10u;

@compute @workgroup_size(WG_X, WG_Y, WG_Z)
fn main(
    // HLSL:87  uint thread_index : SV_GroupIndex
    //   SV_GroupIndex and WGSL's local_invocation_index are defined
    //   identically: tid.x + tid.y*sizeX + tid.z*sizeX*sizeY.
    @builtin(local_invocation_index) thread_index: u32,
    // HLSL:87  uint3 group_id : SV_GroupID
    @builtin(workgroup_id) group_id: vec3<u32>,
    @builtin(num_workgroups) n_groups: vec3<u32>,
) {
    // HLSL:88  uint group_idx = group_id.x + group_id.y*10 + group_id.z*100;
    // HLSL:89  uint idx = thread_index + 1000 * group_idx;
    //   The literals 10/100/1000 are the original dispatch's
    //   num_workgroups.x / (num_workgroups.x * num_workgroups.y) / group size
    //   (main.cpp:1003 dispatches (10, 10, grid_z)). Written generically the
    //   expression reduces to exactly the HLSL constants for that dispatch,
    //   and stays a bijection onto [0, total_invocations) under any reshape —
    //   which is all the algorithm needs, since `idx` is only ever used as a
    //   particle index and agents are mutually independent.
    let group_idx = group_id.x
                  + group_id.y * n_groups.x
                  + group_id.z * n_groups.x * n_groups.y;
    let idx = thread_index + (WG_X * WG_Y * WG_Z) * group_idx;

    // QUIRK(dispatch_truncation): kept for VAC parity
    //   HOST-SIDE, documented here for the reader. main.cpp:1002 computes
    //   grid_z = (NUM_PARTICLES / 100) / 1000, so the dispatch covers only
    //   floor(NUM_PARTICLES / 100000) * 100000 particles; the trailing
    //   NUM_PARTICLES % 100000 never run. Harmless in practice because the
    //   data-representing points live at the front of the buffer and always
    //   execute. There is deliberately NO bounds guard here — adding one
    //   would change nothing, and the original has none.

    // HLSL:92-94  Fetch current particle state
    var x = particles_x[idx];
    var y = particles_y[idx];
    var z = particles_z[idx];

    // HLSL:95-99
    //   Seeding hazard carried over verbatim: `uint(x*y*z)` is a f32->u32
    //   truncation of a product that is 0 on any grid plane (x, y or z == 0)
    //   and that would become indeterminate for grids where x*y*z >= 2^32
    //   (safe for the 712x1200x728 VAC grid: max ~6.2e8, and for 1024^3:
    //   ~1.07e9). f32->u32 out-of-range is "indeterminate" in WGSL and
    //   "undefined" in HLSL — same class of hazard, same practical range.
    var rng: RNG;
    set_seed(&rng,
        wang_hash(73u * idx),                                           // HLSL:97
        wang_hash(u32(x * y * z)));                                     // HLSL:98

    var th = particles_theta[idx];                                      // HLSL:101
    var ph = particles_phi[idx];                                        // HLSL:102
    var particle_weight = particles_weights[idx];                       // HLSL:103

    // -----------------------------------------------------------------------
    // HLSL:105-114  Handle data-representing agents first
    // -----------------------------------------------------------------------
    let is_data = (th < -1.0);                                          // HLSL:106
    if (is_data) {                                                      // HLSL:107
        if (!IGNORE_DATA) {                                             // HLSL:108
            let deposit = 10.0 * particle_weight;                       // HLSL:109
            var color: f32;                                             // HLSL:110
            if (ph < -0.001) { color = -1.0; }
            else if (ph > 0.001) { color = 1.0; }
            else { color = 0.0; }

            // HLSL:111  tex_deposit[uint3(x,y,z)] += float2(deposit, color*deposit);
            // QUIRK(nonatomic_deposit_accumulation): kept for VAC parity
            // QUIRK(r16f_channel_truncation): kept for VAC parity
            //   .y is computed and stored but the bound R16_FLOAT / r32float
            //   texture has no second channel, so `color` is discarded by the
            //   hardware. Kept so the VELOCITY/HALO_COLOR regimes only need a
            //   format widening, not a code change.
            // NOT wrapped with load_deposit_oob_zero (QUIRK(oob_load_zero_emulation)
            // deliberately not applied here): `c` is unwrapped raw position (this is
            // exactly Task 4 Test B's OOB case, e.g. c.x == world_width). `prev` only
            // ever feeds `sum`, which only ever feeds the textureStore on the SAME
            // coordinate `c` immediately below — and Task 4 measured that store as
            // unconditionally discarded on this build/spec when `c` is OOB (hard
            // "will not be executed" guarantee). So whatever garbage-vs-zero `prev`
            // reads in the OOB case is provably dead: it can never reach memory.
            // Guarding it would be inert defensive cost with no behavioural effect.
            let c = vec3<u32>(vec3<f32>(x, y, z));
            let prev = textureLoad(tex_deposit, c).xy;   // r32float -> (r, 0)
            let sum = prev + vec2<f32>(deposit, color * deposit);
            textureStore(tex_deposit, c, vec4<f32>(sum, 0.0, 0.0));
        }
        return;                                                         // HLSL:113
    }

    // HLSL:117  direction of travel
    let center_axis = vec3<f32>(sin(th) * cos(ph), cos(th), sin(th) * sin(ph));

    // HLSL:121-123  off-centre sensing base direction
    //   RNG DRAW #1. The 0.95..1.05 jitter is reused for BOTH the sensing
    //   cone (here) and the turn magnitude (HLSL:161) — a single shared
    //   random, not two independent ones.
    let xiDirectional = 0.95 + 0.1 * random_float(&rng);                // HLSL:121
    let sense_theta = th - cfg.sense_spread * xiDirectional;            // HLSL:122
    let off_center_base_dir = vec3<f32>(
        sin(sense_theta) * cos(ph), cos(sense_theta), sin(sense_theta) * sin(ph)); // HLSL:123

    // HLSL:125-134  Probabilistic (Maxwell-Boltzmann) sensing distance
    var sense_distance_prob = cfg.sense_distance;                       // HLSL:126
    var xi: f32;
    if (FIXED_AGENT_DISTANCE_SAMPLING) {
        // HLSL:128 — consumes NO RNG draw, exactly like the #ifdef branch.
        xi = clamp(mod_floor(0.01 * f32(idx), 1.0), 0.001, 0.999);
    } else {
        // HLSL:130 — RNG DRAW #2.
        xi = clamp(random_float(&rng), 0.001, 0.999);
    }
    // HLSL:132  Maxwell-Boltzmann inverse CDF fit. Argument of log() stays
    // strictly positive over xi in [0.001, 0.999] (checked: 1.37e-4 .. 0.920),
    // so no NaN — but it is numerically delicate near xi -> 1.
    let distance_scaling_factor =
        -0.3033 * log((pow(xi + 0.005, -0.4) - 0.9974) / 7.326);        // HLSL:132
    sense_distance_prob = sense_distance_prob * distance_scaling_factor; // HLSL:134

    // HLSL:136-140  Sample the environment along the movement axis
    // QUIRK(int3_truncated_sensing): kept for VAC parity
    let p = vec3<i32>(vec3<f32>(x, y, z));                              // HLSL:137

    if (QUIRK_DEAD_CURRENT_DEPOSIT_READ) {
        // QUIRK(dead_current_deposit_read): kept for VAC parity
        // HLSL:138 — value is discarded; HLSL:230 recomputes it.
        // Guarded (QUIRK(oob_load_zero_emulation)): `p` is this invocation's
        // PRE-move position, normally in-range but per HLSL:220-223's own
        // comment can round to exactly the grid dimension from a PRIOR
        // iteration's mod_floor. The read result is unused either way (that's
        // the point of this quirk), but the load itself must still be a
        // spec/D3D11-shaped OOB access, not a stray in-bounds memory read.
        _ = load_deposit_oob_zero(p).x;
    }

    let center_sense_pos = center_axis * sense_distance_prob;           // HLSL:139
    // QUIRK(nonperiodic_sensing): kept for VAC parity
    //   Sensing does NOT wrap (unlike movement, HLSL:221-223). Coordinates
    //   outside the grid fall through to the OOB path. D3D11 typed-UAV OOB
    //   loads return 0. WGSL's textureLoad OOB fallback is only PERMITTED to
    //   be zero (spec: "the data for some texel within bounds of the
    //   texture" is an equally legal choice) — Task 4 measured this Dawn/
    //   Metal build actually clamping to an in-bounds texel instead of
    //   zero-filling. QUIRK(oob_load_zero_emulation): both sensing loads
    //   below are explicitly guarded with load_deposit_oob_zero to restore
    //   the D3D11-parity zero-fill this quirk's portability claim depends on.
    var deposit_ahead: f32;
    if (QUIRK_INT3_TRUNCATED_SENSING) {
        deposit_ahead = load_deposit_oob_zero(p + vec3<i32>(center_sense_pos)).x; // HLSL:140
    } else {
        deposit_ahead = load_deposit_oob_zero(p + vec3<i32>(round(center_sense_pos))).x;
    }

    // HLSL:142-145  Stochastic MC direction sampling.  RNG DRAW #3.
    let random_angle = random_float(&rng) * TWOPI - PI;                 // HLSL:143
    let sense_offset = rotate(off_center_base_dir, center_axis, random_angle)
                     * sense_distance_prob;                             // HLSL:144
    var sense_deposit: f32;
    if (QUIRK_INT3_TRUNCATED_SENSING) {
        sense_deposit = load_deposit_oob_zero(p + vec3<i32>(sense_offset)).x;     // HLSL:145
    } else {
        sense_deposit = load_deposit_oob_zero(p + vec3<i32>(round(sense_offset))).x;
    }

    let sharpness = cfg.move_sense_coef;                                // HLSL:146
    var p_straight: f32;
    var p_turn: f32;
    if (PROBABILISTIC_SAMPLING) {
        p_straight = pow(max(deposit_ahead, 0.0), sharpness);           // HLSL:148
        p_turn     = pow(max(sense_deposit, 0.0), sharpness);           // HLSL:149
    } else {
        p_straight = deposit_ahead;                                     // HLSL:151
        p_turn     = sense_deposit;                                     // HLSL:152
    }

    // HLSL:154  RNG DRAW #4 — drawn UNCONDITIONALLY, before the branch, so
    // the stream advances identically whether or not the agent turns.
    let xiDir = random_float(&rng);

    // HLSL:155-166. The HLSL has a brace-less outer `if` wrapping an inner
    // `if`; WGSL requires braces but the nesting is the same.
    if (p_straight + p_turn > 1.0e-5) {                                 // HLSL:155
        var take_turn: bool;
        if (PROBABILISTIC_SAMPLING) {
            take_turn = xiDir < p_turn / (p_turn + p_straight);         // HLSL:157
        } else {
            take_turn = p_turn > p_straight;                            // HLSL:159
        }
        if (take_turn) {
            let theta_turn = th - cfg.turn_angle * xiDirectional;       // HLSL:161
            let off_center_base_dir_turn = vec3<f32>(
                sin(theta_turn) * cos(ph), cos(theta_turn), sin(theta_turn) * sin(ph)); // HLSL:162
            // The SAME `random_angle` as the sensing ray — the agent turns
            // toward exactly the direction it sensed. Intentional.
            let new_direction = rotate(off_center_base_dir_turn, center_axis, random_angle); // HLSL:163
            ph = atan2(new_direction.z, new_direction.x);               // HLSL:164
            // acos() can see |arg| marginally > 1 from f32 rounding, giving
            // NaN in HLSL and an indeterminate value in WGSL. Not clamped in
            // the original; not clamped here.
            th = acos(new_direction.y / length(new_direction));         // HLSL:165
        }
    }

    // HLSL:168-196  the "Naive 3D sampling" block is commented out in the
    // original and is intentionally NOT ported. DIR_SAMPLE_POINTS/HALFPI
    // exist only for it.

    // -----------------------------------------------------------------------
    // HLSL:198-212  Rotation applied by the pull toward the world centre.
    // Uniform-controlled branch; free in practice. center_attraction is 0 in
    // the SDSS VAC parameter set, so this whole block is inert for validation.
    // -----------------------------------------------------------------------
    if (cfg.center_attraction > 0.001) {                                // HLSL:199
        // CAREFUL: HLSL `world_width / 2.0` promotes the int to float BEFORE
        // dividing. Writing f32(cfg.world_width / 2) here would be integer
        // division and would silently shift the attractor by half a cell on
        // odd grid dimensions.
        let to_center = vec3<f32>(
            f32(cfg.world_width)  / 2.0 - x,
            f32(cfg.world_height) / 2.0 - y,
            f32(cfg.world_depth)  / 2.0 - z);                           // HLSL:200
        let d_center = length(to_center);                               // HLSL:201
        let d_c_turn = clamp((d_center - 50.0) / 150.0, 0.0, 1.0)
                     * cfg.center_attraction;                           // HLSL:202
        var dir = vec3<f32>(sin(th) * cos(ph), cos(th), sin(th) * sin(ph)); // HLSL:203
        let center_dir = normalize(to_center);                          // HLSL:204
        // HLSL:205 declares `float3 center_angle = acos(<scalar>)`, i.e. an
        // implicit scalar->vector broadcast. Kept as a vec3 for literalness;
        // all three components are equal so the slerp below is unaffected.
        let center_angle = vec3<f32>(acos(dot(dir, center_dir)));       // HLSL:205
        let st = 0.1 * d_c_turn;                                        // HLSL:206
        // Divides by sin(center_angle), which is 0 when the agent already
        // points at the centre -> inf/NaN. Unguarded in the original.
        dir = sin((1.0 - st) * center_angle) / sin(center_angle) * dir
            + sin(st * center_angle) / sin(center_angle) * center_dir;  // HLSL:207
        if (length(dir) > 0.0 && (dir.z != 0.0 || dir.x != 0.0)) {      // HLSL:208
            th = acos(dir.y / length(dir));                             // HLSL:209
            ph = atan2(dir.z, dir.x);                                   // HLSL:210
        }
    }

    // -----------------------------------------------------------------------
    // HLSL:214-218  Make a step
    // QUIRK(move_sense_distance_coupling): kept for VAC parity
    //   The step length is scaled by (0.1 + 0.9 * distance_scaling_factor),
    //   reusing the Maxwell-Boltzmann draw made for SENSING. Movement and
    //   sensing distances are therefore correlated per agent per step. This
    //   is load-bearing MCPM behaviour, not an accident, but it is the kind
    //   of coupling a "clean" rewrite would break, so it is marked.
    // -----------------------------------------------------------------------
    let dp = vec3<f32>(sin(th) * cos(ph), cos(th), sin(th) * sin(ph))
           * cfg.move_distance
           * (0.1 + 0.9 * distance_scaling_factor);                     // HLSL:215
    x = x + dp.x;                                                       // HLSL:216
    y = y + dp.y;                                                       // HLSL:217
    z = z + dp.z;                                                       // HLSL:218

    // HLSL:220-223  Keep the particle inside the environment.
    // Floor-mod => fully periodic on BOTH sides (contrast the decay shader).
    // Note the residual f32 edge case, present in the original too: for a
    // tiny negative x, x - w*floor(x/w) can round up to exactly w, putting
    // the agent one cell out of bounds for the rest of this invocation.
    x = mod_floor(x, f32(cfg.world_width));                             // HLSL:221
    y = mod_floor(y, f32(cfg.world_height));                            // HLSL:222
    z = mod_floor(z, f32(cfg.world_depth));                             // HLSL:223

    // -----------------------------------------------------------------------
    // HLSL:225-245  Inactivity check / rerouting
    // -----------------------------------------------------------------------
    // HLSL uses `const` here for values that depend on a uniform. WGSL `const`
    // requires a const-expression, so these become `let`.
    const w_f: f32 = 0.9;                                               // HLSL:226
    let n_agents_M = f32(cfg.n_agents) / 1.0e6;                         // HLSL:227
    // HLSL:228 keeps an older, 5x larger threshold commented out.
    let thr_f = 0.05 * n_agents_M * cfg.deposit_value
              + 0.1e-3 * n_agents_M;                                    // HLSL:229

    // Guarded (QUIRK(oob_load_zero_emulation)): x/y/z were just mod_floor'd
    // above and are normally in [0, dim), but HLSL:220-223's own comment
    // flags the residual f32 rounding hazard that can land this exactly on
    // the grid dimension (one past the last valid index) — see Task 4's
    // sim_kernel_tests.cpp Test B, which exercises precisely this class of
    // edge coordinate (via the data-point path, not this one) and confirms
    // the store side discards cleanly; this load side needs the explicit
    // zero-fill guard since textureLoad's OOB fallback is not.
    let current_deposit = load_deposit_oob_zero(vec3<i32>(vec3<f32>(x, y, z))).x; // HLSL:230
    particle_weight = w_f * particle_weight + (1.0 - w_f) * current_deposit;         // HLSL:231

    if (AGENT_REROUTING) {                                              // HLSL:232
        if (particle_weight < thr_f) {                                  // HLSL:233
            // RNG DRAWS #5, #6, #7 — only consumed on the reroute path, so
            // rerouted and surviving agents diverge in stream position.
            // Faithful: the HLSL is identical.
            x = random_float(&rng) * f32(cfg.world_width);              // HLSL:234
            y = random_float(&rng) * f32(cfg.world_height);             // HLSL:235
            z = random_float(&rng) * f32(cfg.world_depth);              // HLSL:236
            // HLSL:237-242 keep an "Experimental" data-seeded respawn
            // commented out; not ported.
            particle_weight = cfg.deposit_value;                        // HLSL:243
        }
    }

    // HLSL:247-253  Update particle state
    particles_x[idx]       = x;                                         // HLSL:248
    particles_y[idx]       = y;                                         // HLSL:249
    particles_z[idx]       = z;                                         // HLSL:250
    particles_theta[idx]   = th;                                        // HLSL:251
    particles_phi[idx]     = ph;                                        // HLSL:252
    particles_weights[idx] = particle_weight;                           // HLSL:253

    let wc = vec3<u32>(vec3<f32>(x, y, z));

    // HLSL:255  tex_deposit[uint3(x,y,z)] += float2(deposit_value, 0.0);
    // QUIRK(nonatomic_deposit_accumulation): kept for VAC parity
    //   Note this is a SECOND read-modify-write of the same texel this
    //   invocation already read at HLSL:230 — the value read there is stale
    //   with respect to concurrent writers, by design.
    {
        // Guarded read (QUIRK(oob_load_zero_emulation), same f32-rounding
        // hazard as HLSL:230 above — `wc` is derived from the same
        // post-mod_floor x/y/z). The store on the next line is UNCHANGED:
        // Task 4 measured OOB textureStore as correctly/unconditionally
        // discarded on this build, so `wc` itself needs no guard, only the
        // load that feeds `sum`.
        let prev = load_deposit_oob_zero(vec3<i32>(wc)).xy;
        let sum = prev + vec2<f32>(cfg.deposit_value, 0.0);
        textureStore(tex_deposit, wc, vec4<f32>(sum, 0.0, 0.0));
    }

    // HLSL:257 keeps the scalar-only trace accumulation commented out.
    // HLSL:258  tex_trace[uint3(x,y,z)] += float4(t, |cx|, |cy|, |cz|);
    // QUIRK(nonatomic_trace_accumulation): kept for VAC parity
    // QUIRK(r16f_channel_truncation): kept for VAC parity
    //   .yzw (the direction histogram used by VELOCITY_ANALYSIS) are computed
    //   and discarded by the single-channel format in this regime. The
    //   exported/validated cube is .x only.
    // QUIRK(trace_weighted_by_sense_distance): kept for VAC parity
    //   The trace increment is distance_scaling_factor / normalization_factor,
    //   i.e. an agent that sensed far deposits MORE trace, even though the
    //   trace is nominally an occupancy field. normalization_factor is pinned
    //   to 1.0 at main.cpp:803 (the adaptive update at main.cpp:1315 is
    //   commented out), so this reduces to a bare distance weighting.
    {
        // Guarded read (QUIRK(oob_load_zero_emulation), same rationale as the
        // tex_deposit RMW above — trace's own binding, hence the separate
        // load_trace_oob_zero helper). Store is UNCHANGED for the same
        // discard-verified-by-Task-4 reason.
        let prev = load_trace_oob_zero(vec3<i32>(wc));
        let add = vec4<f32>(
            (1.0 / cfg.normalization_factor) * distance_scaling_factor,
            abs(center_axis.x), abs(center_axis.y), abs(center_axis.z));
        textureStore(tex_trace, wc, prev + add);
    }
}
