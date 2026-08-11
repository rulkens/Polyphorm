// GPU micro-tests pinning port quirks that are INVISIBLE in any rendered
// image (translation-notes.md §7.9):
//   Test A: decay boundary — QUIRK(decay_weight_all_int3) +
//           QUIRK(nonperiodic_low_boundary) + the uncompensated denominator
//           darkening, all exercised simultaneously (D2/D4/D5/A4).
//   Test B: OOB storage-texture writes are DISCARDED (WGSL spec), not
//           clamped (a divergence some D3D11 UAV implementations could
//           exhibit differently) — translation-notes.md §7.8.
//
// These are the port's only guard for behaviours the whole M5 VAC
// validation effort depends on: nothing about them shows up in a rendered
// image, only in the raw voxel values.
#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace graphics;

// SimulationConfig must byte-match main.cpp's (16 scalars, 64 B) and the
// WGSL `struct SimulationConfig` declared identically in both
// cs_field_decay.wgsl and cs_agents_propagate.wgsl.
struct SimulationConfig {
    float sense_spread, sense_distance, turn_angle, move_distance;
    float deposit_value, decay_factor, center_attraction; int world_width;
    int world_height, world_depth; float move_sense_coef, normalization_factor;
    int n_data_points, n_agents, n_iteration, filler3;
};
static_assert(sizeof(SimulationConfig) == 64, "config layout");

static bool approx(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static char *load_shader_file(const char *path, File *out) {
    *out = file_system::read_file(path);
    assert(out->data != NULL && "shader file missing");
    return (char *)out->data;
}

// ProbeCfg must byte-match shaders/tests/probe3d.wgsl's `struct ProbeCfg`.
struct ProbeCfg { int32_t n_probes, slab_x, dim_y, dim_z; };

// probe(): loads shaders/tests/probe3d.wgsl (once, cached in probe_cs),
// uploads `coords` (xyz triples, coords.size() == 3*n_probes) via a
// structured buffer, optionally sums the x==slab_x slab (dim_y x dim_z),
// and reads back n_probes + (slab_x>=0 ? 1 : 0) floats into `out`.
// Follows full bind discipline: cfg at group0 slot0, group1 slots 0/1/2,
// unset everything after (unset_texture_compute for the texture,
// unset_structured_buffer for the two buffers).
static void probe(ComputeShader *probe_cs, Texture3D *tex,
                   const std::vector<int32_t> &coords, int n_probes,
                   int slab_x, int dim_y, int dim_z, float *out) {
    assert((int)coords.size() == 3 * n_probes);

    ProbeCfg cfg = { n_probes, slab_x, dim_y, dim_z };
    ConstantBuffer cfg_buf = get_constant_buffer(sizeof(ProbeCfg));
    update_constant_buffer(&cfg_buf, &cfg);

    // Storage buffers must be non-empty even when unused (n_probes == 0).
    std::vector<int32_t> coord_data = coords;
    if (coord_data.empty()) coord_data.push_back(0);
    StructuredBuffer coord_buf =
        get_structured_buffer(sizeof(int32_t), (uint32_t)coord_data.size());
    update_structured_buffer(&coord_buf, coord_data.data());

    int n_results = n_probes + (slab_x >= 0 ? 1 : 0);
    int n_results_alloc = n_results > 0 ? n_results : 1;
    StructuredBuffer result_buf =
        get_structured_buffer(sizeof(float), (uint32_t)n_results_alloc);

    set_compute_shader(probe_cs);
    set_constant_buffer(&cfg_buf, 0);
    set_texture_compute(tex, 0);
    set_structured_buffer(&coord_buf, 1);
    set_structured_buffer(&result_buf, 2);
    run_compute(1, 1, 1);
    unset_texture_compute(0);
    unset_structured_buffer(1);
    unset_structured_buffer(2);

    std::vector<float> results(n_results_alloc);
    capture_structured_buffer(&result_buf, results.data(), (uint32_t)n_results_alloc, sizeof(float));
    for (int i = 0; i < n_results; i++) out[i] = results[i];

    release(&cfg_buf);
    release(&coord_buf);
    release(&result_buf);
}

int main() {
    bool ok = init();   // headless: no init_swap_chain (matches shader_compile_tests.cpp)
    assert(ok);

    File probe_f;
    char *probe_code = load_shader_file(SHADER_DIR "/tests/probe3d.wgsl", &probe_f);
    ComputeShader probe_cs = get_compute_shader_from_code(probe_code, probe_f.size);
    assert(is_ready(&probe_cs));
    file_system::release_file(probe_f);

    // =========================================================================
    // Test A — decay boundary (pins D2/D4/D5/A4 simultaneously).
    // Expected values are exact math (translation-notes.md §7.9), DERIVED
    // ASSUMING textureLoad on an out-of-bounds texel address returns the
    // zero value:
    //   interior (8,8,8): all 27 taps present -> 1.0 * DECAY
    //   x=0 face (0,8,8): low side is absorbing (QUIRK(nonperiodic_low_boundary)):
    //     the -1 taps fall out of bounds -> 0, but the denominator w does NOT
    //     compensate (always sums 23.6188021, the QUIRK(decay_weight_all_int3)
    //     total) -> (23.6188021 - 7.3094011) / 23.6188021 * DECAY = 0.6905262 * DECAY
    //   x=W-1 face (15,8,8): the high side wraps (p % W == 0 for p == W) -> all
    //     27 taps present -> 1.0 * DECAY
    //
    // FINDING (see the DIAGNOSTIC block below, which measures this directly):
    // that "returns zero" assumption — stated as a hard guarantee in
    // cs_field_decay.wgsl's QUIRK(nonperiodic_low_boundary) comment and in
    // translation-notes.md §7.9 ("WGSL guarantees textureLoad on an invalid
    // texel address returns the zero value") — is FALSE. The current W3C WGSL
    // spec (17.7.4 textureLoad, "Out-of-bounds" clause) states an invalid
    // logical texel address makes the built-in return ONE OF: (a) the zero
    // vector, OR (b) "the data for some texel within bounds of the texture" —
    // implementation's choice, unspecified which. It is NOT a zero guarantee.
    // (Contrast textureStore, Test B below: for stores, an invalid address is
    // a hard "will not be executed" per spec — that half of the assumption
    // DOES hold, and Test B pins it.)
    //
    // On the pinned Dawn build (v20260807.193620) / Metal backend this test
    // suite runs on, the measured choice is: OOB coordinates on ANY axis, in
    // EITHER direction, clamp to the texture's highest valid index on that
    // axis (verified with x=-1, x=-100, and x=16 against a texture with
    // distinct markers at x=0 and x=W-1 — all three return the x=W-1 marker,
    // not the x=0 marker, and not zero). This is spec-legal but is NOT the
    // D3D11-parity behaviour the shader's own comments assume, so the boundary
    // face values below deviate from the brief's derived math on this
    // hardware. Per the task's instructions this is investigated (see the
    // diagnostic evidence), and is neither a shader logic bug nor a test
    // harness bug — it is a genuine WGSL-portability gap in Task 3's shader
    // that only this kind of raw-voxel probe can surface. The assertion below
    // is therefore LEFT AS THE BRIEF SPECIFIES (unweakened, tolerance 1e-5)
    // and is expected to FAIL on this hardware/Dawn build; see the task
    // report for the BLOCKED writeup. Loosening it or reshaping it to match
    // today's observed clamp would just pin an unspecified, non-portable
    // implementation choice that spec-conformant Dawn/Metal updates are free
    // to change out from under us.
    // =========================================================================
    {
        const int W = 16;
        const float DECAY = 0.9f;

        Texture3D tex_in  = get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
        Texture3D tex_out = get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
        Texture3D trace   = get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
        assert(is_ready(&tex_in) && is_ready(&tex_out) && is_ready(&trace));
        clear_texture(&tex_in, 1.0f);
        clear_texture(&tex_out, 0.0f);
        clear_texture(&trace, 0.0f);

        // DIAGNOSTIC: raw OOB textureLoad probe — distinguishes clamp-to-edge
        // vs periodic-wrap vs spec-legal zero by using a texture with DISTINCT
        // values at x=0 (2.0) and x=W-1 (3.0), rest 1.0. This is the evidence
        // for the FINDING documented in the Test A header comment above: on
        // this hardware/Dawn build, OOB textureLoad clamps to the far (max
        // index) edge rather than returning zero, for negative AND
        // past-the-end coordinates alike.
        {
            std::vector<float> data((size_t)W*W*W, 1.0f);
            auto idx3 = [&](int x,int y,int z){ return (size_t)z*W*W + (size_t)y*W + (size_t)x; };
            data[idx3(0,8,8)] = 2.0f;
            data[idx3(W-1,8,8)] = 3.0f;
            Texture3D diag_tex = get_texture3D(data.data(), W, W, W, Format::R32_FLOAT, 4);
            float diag[3] = {-999,-999,-999};
            std::vector<int32_t> diag_coords = {-1,8,8,  -100,8,8,  16,8,8};
            probe(&probe_cs, &diag_tex, diag_coords, 3, -1, 0, 0, diag);
            printf("DIAG: textureLoad OOB probes: x=-1 -> %.7f, x=-100 -> %.7f, x=W(=16) -> %.7f "
                   "(x=0 voxel=2.0, x=W-1 voxel=3.0, elsewhere=1.0; spec says all three should be 0.0)\n",
                   diag[0], diag[1], diag[2]);
            release(&diag_tex);
        }

        SimulationConfig cfg = {};
        cfg.decay_factor = DECAY;
        cfg.world_width = W; cfg.world_height = W; cfg.world_depth = W;
        ConstantBuffer cfg_buf = get_constant_buffer(sizeof(SimulationConfig));
        update_constant_buffer(&cfg_buf, &cfg);

        File f = file_system::read_file(SHADER_DIR "/cs_field_decay.wgsl");
        assert(f.data != NULL);
        // Pin the quirk toggles explicitly (all default to true in the shader
        // source, but stating them here makes the test's assumptions robust
        // to a future default change in cs_field_decay.wgsl).
        ShaderConstant decay_consts[] = {
            {"QUIRK_DECAY_WEIGHT_ALL_INT3", 1.0},
            {"QUIRK_NONPERIODIC_LOW_BOUNDARY", 1.0},
            {"QUIRK_DITHERED_TRACE_DECAY", 1.0},
            {"QUIRK_RNG_SEED_GUARD_TYPO", 1.0},
        };
        ComputeShader decay = get_compute_shader_from_code((char *)f.data, f.size, decay_consts, 4);
        file_system::release_file(f);
        assert(is_ready(&decay));

        set_compute_shader(&decay);
        set_constant_buffer(&cfg_buf, 0);
        set_texture_compute(&tex_in, 0);
        set_texture_compute(&tex_out, 1);
        set_texture_compute(&trace, 2);
        run_compute(W / 8, W / 8, W / 8);
        unset_texture_compute(0);
        unset_texture_compute(1);
        unset_texture_compute(2);

        float vals[3] = {0, 0, 0};
        std::vector<int32_t> coords = {8,8,8, 0,8,8, 15,8,8};
        probe(&probe_cs, &tex_out, coords, /*n_probes*/3, /*slab_x*/-1, W, W, vals);

        printf("Test A (decay boundary): interior=%.7f (expect %.7f) "
               "x=0 face=%.7f (expect %.7f) x=W-1 face=%.7f (expect %.7f)\n",
               vals[0], 1.0f * DECAY,
               vals[1], 0.6905262f * DECAY,
               vals[2], 1.0f * DECAY);

        assert(approx(vals[0], 1.0f * DECAY, 1e-5f));
        // EXPECTED TO FAIL on this hardware/Dawn build — see the FINDING in
        // this block's header comment and the DIAGNOSTIC probe above. Kept
        // exactly as the brief derives it (unweakened, 1e-5 tolerance): the
        // measured x=0 face value does not match because textureLoad's
        // out-of-bounds fallback is spec-implementation-defined, not
        // guaranteed-zero, and this Dawn/Metal build's choice (clamp to the
        // far edge) differs from the zero-fill the shader's own quirk
        // documentation assumes. NOT loosened per task instructions; see the
        // task report for the BLOCKED writeup.
        assert(approx(vals[1], 0.6905262f * DECAY, 1e-5f));
        assert(approx(vals[2], 1.0f * DECAY, 1e-5f));
        printf("sim_kernel_tests: Test A (decay boundary quirks D2/D4/D5/A4) passed\n");

        release(&decay);
        release(&cfg_buf);
        release(&tex_in); release(&tex_out); release(&trace);
    }

    // =========================================================================
    // Test B — OOB store is discarded, not clamped (translation-notes.md §7.8).
    //
    // The brief's sketch assumed the data-point deposit write goes through the
    // same floor-mod wrap as agent movement (HLSL:221-223 / cs_agents_propagate
    // .wgsl mod_floor), so a value like x = -1e-7f could be crafted to wrap to
    // exactly W under floor-mod at f32 precision. Reading the actual DATA-POINT
    // branch (is_data = th < -1.0, cs_agents_propagate.wgsl lines ~298-320)
    // shows this assumption does not hold: the data-point deposit write coord
    // is `vec3<u32>(vec3<f32>(x, y, z))` — a DIRECT float->u32 conversion with
    // NO mod_floor wrap at all (mod_floor is only applied to agents that fall
    // through to the movement path below the early `return` at HLSL:113 /
    // the WGSL equivalent).
    //
    // WGSL's f32->u32 conversion rounds toward zero then clamps to the u32
    // range, so it is exact and deterministic (no epsilon/rounding hazard) for
    // x = 16.0f (== W): u32(16.0f) == 16u == W, one texel past the last valid
    // index (W-1 == 15). That is a clean, hardware-independent way to place
    // the OOB write exactly at the edge this test needs to probe — a stronger
    // construction than the brief's near-zero-negative trick, which (a) only
    // ever converts to 0 (in-bounds) on this code path, since negative floats
    // round-toward-zero to 0 or clamp to 0 in WGSL's f32->u32 conversion, and
    // (b) was designed against the mod_floor wrap that this code path does not
    // use.
    //
    // Note also: the data-point deposit MAGNITUDE is `10.0 * particle_weight`
    // (hardcoded 10.0, NOT cfg.deposit_value) — so with weight = 1.0 the would-
    // be deposit is 10.0, not 7.0 as the brief's illustrative comment assumed.
    // cfg.deposit_value is still set to 7.0f per the brief (harmless: unused
    // by the data-point path) so a stray agent-path write would be
    // distinguishable from a data-point write if one ever occurred.
    // =========================================================================
    {
        const int W = 16;

        Texture3D tex_deposit = get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
        Texture3D tex_trace   = get_texture3D(NULL, W, W, W, Format::R32_FLOAT, 4);
        assert(is_ready(&tex_deposit) && is_ready(&tex_trace));
        clear_texture(&tex_deposit, 0.0f);
        clear_texture(&tex_trace, 0.0f);

        SimulationConfig cfg = {};
        cfg.n_data_points = 1;
        cfg.n_agents = 0;
        cfg.deposit_value = 7.0f;
        cfg.move_distance = 0.0f;
        cfg.sense_distance = 0.0f;
        cfg.sense_spread = 0.0f;
        cfg.turn_angle = 0.0f;
        cfg.move_sense_coef = 1.0f;
        cfg.normalization_factor = 1.0f;
        cfg.world_width = W; cfg.world_height = W; cfg.world_depth = W;
        ConstantBuffer cfg_buf = get_constant_buffer(sizeof(SimulationConfig));
        update_constant_buffer(&cfg_buf, &cfg);

        // One particle: the data-point marker is theta < -1.0 (brief says
        // "-5.0f", confirmed against cs_agents_propagate.wgsl's
        // `let is_data = (th < -1.0);`). x = W (== 16.0f) exactly, so the
        // direct-cast deposit write coordinate lands at (W, 8, 8) — one texel
        // past the last valid x index (W-1 == 15) — a deterministic OOB store.
        float px = 16.0f, py = 8.0f, pz = 8.0f, pphi = 0.0f, ptheta = -5.0f, pweight = 1.0f;
        StructuredBuffer buf_x     = get_structured_buffer(sizeof(float), 1);
        StructuredBuffer buf_y     = get_structured_buffer(sizeof(float), 1);
        StructuredBuffer buf_z     = get_structured_buffer(sizeof(float), 1);
        StructuredBuffer buf_phi   = get_structured_buffer(sizeof(float), 1);
        StructuredBuffer buf_theta = get_structured_buffer(sizeof(float), 1);
        StructuredBuffer buf_weight= get_structured_buffer(sizeof(float), 1);
        update_structured_buffer(&buf_x, &px);
        update_structured_buffer(&buf_y, &py);
        update_structured_buffer(&buf_z, &pz);
        update_structured_buffer(&buf_phi, &pphi);
        update_structured_buffer(&buf_theta, &ptheta);
        update_structured_buffer(&buf_weight, &pweight);

        File f = file_system::read_file(SHADER_DIR "/cs_agents_propagate.wgsl");
        assert(f.data != NULL);
        ComputeShader propagate = get_compute_shader_from_code((char *)f.data, f.size);
        file_system::release_file(f);
        assert(is_ready(&propagate));

        set_compute_shader(&propagate);
        set_constant_buffer(&cfg_buf, 0);
        set_texture_compute(&tex_deposit, 0);
        set_texture_compute(&tex_trace, 1);
        set_structured_buffer(&buf_x, 2);
        set_structured_buffer(&buf_y, 3);
        set_structured_buffer(&buf_z, 4);
        set_structured_buffer(&buf_phi, 5);
        set_structured_buffer(&buf_theta, 6);
        set_structured_buffer(&buf_weight, 7);
        // WG_X/Y/Z default to 10 (1000 invocations/group); the shader's own
        // index math + robustness-clamped OOB buffer reads make invocations
        // beyond idx==0 either re-read particle 0's data (redundant, harmless
        // data-point writes to the same OOB coord) or read zeroed defaults
        // (theta==0 is not < -1.0, so they fall to the agent path with
        // move_distance==0 / sense_*==0 and are inert); neither path can ever
        // write to the deposit-texture x==W-1 slab under test.
        run_compute(1, 1, 1);
        unset_texture_compute(0);
        unset_texture_compute(1);
        unset_structured_buffer(2);
        unset_structured_buffer(3);
        unset_structured_buffer(4);
        unset_structured_buffer(5);
        unset_structured_buffer(6);
        unset_structured_buffer(7);

        // Probe the far slab x == W-1 (where a CLAMPED OOB store would land)
        // plus the specific voxel (W-1, 8, 8) for a direct, unambiguous readout.
        float vals[2] = {0, 0};
        std::vector<int32_t> coords = {W - 1, 8, 8};
        probe(&probe_cs, &tex_deposit, coords, /*n_probes*/1, /*slab_x*/W - 1, W, W, vals);
        float voxel_wm1_8_8 = vals[0];
        float slab_wm1_sum  = vals[1];

        printf("Test B (OOB store discard): voxel(W-1,8,8)=%.7f slab[x=W-1] sum=%.7f "
               "(both expect 0.0 if OOB store was discarded)\n",
               voxel_wm1_8_8, slab_wm1_sum);

        if (!approx(slab_wm1_sum, 0.0f, 1e-5f) || !approx(voxel_wm1_8_8, 0.0f, 1e-5f)) {
            printf("BLOCKED: OOB store landed at x=W-1 (voxel=%.7f, slab sum=%.7f). "
                   "Metal CLAMPED the out-of-bounds textureStore instead of discarding "
                   "it, diverging from D3D11's discard semantics assumed by the port. "
                   "This changes M5's validation calculus; reporting evidence, NOT "
                   "loosening the assertion.\n", voxel_wm1_8_8, slab_wm1_sum);
            assert(false && "OOB store was clamped, not discarded (Metal divergence) — see BLOCKED report above");
        }
        assert(approx(slab_wm1_sum, 0.0f, 1e-5f));
        assert(approx(voxel_wm1_8_8, 0.0f, 1e-5f));
        printf("sim_kernel_tests: Test B (OOB store discard) passed — hardware DISCARDED "
               "the out-of-bounds textureStore, matching WGSL spec / D3D11 semantics\n");

        release(&propagate);
        release(&cfg_buf);
        release(&buf_x); release(&buf_y); release(&buf_z);
        release(&buf_phi); release(&buf_theta); release(&buf_weight);
        release(&tex_deposit); release(&tex_trace);
    }

    release(&probe_cs);
    release();
    printf("All sim_kernel_tests passed\n");
    return 0;
}
