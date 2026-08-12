#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

// TEST_SHADER_DIR is injected by CMake as an absolute path to shaders/tests.
static char *load_shader(const char *name, File *out) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", TEST_SHADER_DIR, name);
    *out = file_system::read_file(path);
    assert(out->data && "test shader file missing");
    return (char *)out->data;
}

int main() {
    // --- Test 0 (M5): f32->f16 export conversion, known IEEE 754 vectors.
    // Spec "Testing" requires a CTest unit for the f32->f16 export conversion;
    // vectors cover zero/sign, normals, f16 max, overflow->inf, smallest
    // normal, subnormal, and the round-to-nearest-even tie cases. ---
    {
        struct { float in; uint16_t expect; } vec[] = {
            {0.0f, 0x0000}, {-0.0f, 0x8000},
            {1.0f, 0x3C00}, {-2.0f, 0xC000}, {0.5f, 0x3800},
            {65504.0f, 0x7BFF},                  // f16 max finite
            {65536.0f, 0x7C00},                  // overflow -> +inf
            {-65536.0f, 0xFC00},                 // overflow -> -inf
            {INFINITY, 0x7C00}, {-INFINITY, 0xFC00},
            {6.103515625e-5f, 0x0400},           // smallest normal (2^-14)
            {5.9604644775390625e-8f, 0x0001},    // smallest subnormal (2^-24)
            {1.0009765625f, 0x3C01},             // 1 + 2^-10: exactly representable
            {1.00048828125f, 0x3C00},            // 1 + 2^-11: tie -> even (down)
            {1.00146484375f, 0x3C02},            // 1 + 3*2^-11: tie -> even (up)
        };
        for (auto &t : vec) {
            uint16_t got = graphics::f32_to_f16(t.in);
            if (got != t.expect) {
                fprintf(stderr, "f32_to_f16(%g) = 0x%04X, want 0x%04X\n",
                        (double)t.in, got, t.expect);
                assert(false);
            }
        }
        uint16_t nan_bits = graphics::f32_to_f16(NAN);
        assert((nan_bits & 0x7C00) == 0x7C00 && (nan_bits & 0x03FF) != 0); // any f16 NaN
        printf("graphics_tests: f32_to_f16 known vectors passed\n");
    }

    bool ok = graphics::init();   // headless: no init_swap_chain
    assert(ok);

    // --- Test 1: uniform + storage buffer + override constant + readback ---
    {
        File f;
        char *code = load_shader("test_write_ids.wgsl", &f);
        graphics::ShaderConstant consts[] = {{"MULTIPLIER", 3.0}};
        graphics::ComputeShader cs =
            graphics::get_compute_shader_from_code(code, (uint32_t)f.size, consts, 1);
        assert(graphics::is_ready(&cs));
        file_system::release_file(f);

        const uint32_t N = 1000;
        graphics::StructuredBuffer buf = graphics::get_structured_buffer(sizeof(uint32_t), N);
        graphics::ConstantBuffer cb = graphics::get_constant_buffer(16);
        uint32_t cfg[4] = {5, 7, 0, 0};   // mul=5, add=7
        graphics::update_constant_buffer(&cb, cfg);
        graphics::set_constant_buffer(&cb, 0);
        graphics::set_structured_buffer(&buf, 2);
        graphics::set_compute_shader(&cs);
        graphics::run_compute((N + 63) / 64, 1, 1);
        graphics::unset_texture_compute(2);

        uint32_t result[N];
        graphics::capture_structured_buffer(&buf, result, N, sizeof(uint32_t));
        for (uint32_t i = 0; i < N; i++)
            assert(result[i] == i * 5u * 3u + 7u);
        printf("graphics_tests: write_ids (uniform+override+readback) passed\n");
        graphics::release(&buf); graphics::release(&cb); graphics::release(&cs);
    }

    // --- Test 2: 3D storage texture clear + read into buffer ---
    {
        File f;
        char *code = load_shader("test_tex3d_roundtrip.wgsl", &f);
        graphics::ComputeShader cs =
            graphics::get_compute_shader_from_code(code, (uint32_t)f.size);
        assert(graphics::is_ready(&cs));
        file_system::release_file(f);

        const uint32_t W = 8, H = 8, D = 8;
        graphics::Texture3D tex =
            graphics::get_texture3D(nullptr, W, H, D, graphics::Format::R32_FLOAT);
        assert(graphics::is_ready(&tex));
        graphics::clear_texture(&tex, 2.5f);

        graphics::StructuredBuffer buf =
            graphics::get_structured_buffer(sizeof(float), W * H * D);
        graphics::set_texture_compute(&tex, 0);   // NOTE: binds ua_view; the
        // kernel declares `read` access — if Dawn rejects a read_write-created
        // view bound as read (it should not; access is declared shader-side),
        // record the finding, it affects M2b's decay shader (D1 narrowing).
        graphics::set_structured_buffer(&buf, 1);
        graphics::set_compute_shader(&cs);
        graphics::run_compute(W / 4, H / 4, D / 4);
        graphics::unset_texture_compute(0);
        graphics::unset_texture_compute(1);

        float result[W * H * D];
        graphics::capture_structured_buffer(&buf, result, W * H * D, sizeof(float));
        for (uint32_t i = 0; i < W * H * D; i++)
            assert(result[i] == 2.5f);
        printf("graphics_tests: tex3d clear+roundtrip passed\n");
        graphics::release(&buf); graphics::release(&tex); graphics::release(&cs);
    }

    // --- Test 3: invalid WGSL yields is_ready == false, no crash ---
    {
        char bad[] = "this is not wgsl";
        graphics::ComputeShader cs = graphics::get_compute_shader_from_code(bad, sizeof(bad));
        assert(!graphics::is_ready(&cs));
        printf("graphics_tests: invalid shader rejected cleanly\n");
    }

    // --- Test 4 (C1 regression): update_constant_buffer must not retroactively
    // corrupt a dispatch already recorded (but not yet flushed) into the
    // encoder. queue.WriteBuffer executes in queue order, i.e. before any
    // later-submitted command buffer — so a second WriteBuffer sandwiched
    // between two recorded-but-unflushed dispatches would (without a flush
    // in update_constant_buffer) retroactively apply to BOTH once the
    // encoder eventually gets flushed. This mirrors the D3D11 Map(WRITE_DISCARD)
    // call-order guarantee the fork must preserve. ---
    {
        File f;
        char *code = load_shader("test_write_ids.wgsl", &f);
        graphics::ComputeShader cs =
            graphics::get_compute_shader_from_code(code, (uint32_t)f.size);
        assert(graphics::is_ready(&cs));
        file_system::release_file(f);

        const uint32_t N = 64;
        graphics::StructuredBuffer buf1 = graphics::get_structured_buffer(sizeof(uint32_t), N);
        graphics::StructuredBuffer buf2 = graphics::get_structured_buffer(sizeof(uint32_t), N);
        graphics::ConstantBuffer cb = graphics::get_constant_buffer(16);

        uint32_t cfg1[4] = {5, 7, 0, 0};    // mul=5, add=7
        uint32_t cfg2[4] = {9, 11, 0, 0};   // mul=9, add=11

        graphics::update_constant_buffer(&cb, cfg1);
        graphics::set_constant_buffer(&cb, 0);
        graphics::set_structured_buffer(&buf1, 2);
        graphics::set_compute_shader(&cs);
        graphics::run_compute((N + 63) / 64, 1, 1);   // recorded, NOT yet flushed

        // Hazard: this WriteBuffer must not retroactively apply to the
        // dispatch recorded immediately above.
        graphics::update_constant_buffer(&cb, cfg2);
        graphics::set_structured_buffer(&buf2, 2);
        graphics::run_compute((N + 63) / 64, 1, 1);   // recorded with cfg2 live

        uint32_t result1[N], result2[N];
        graphics::capture_structured_buffer(&buf1, result1, N, sizeof(uint32_t));
        graphics::capture_structured_buffer(&buf2, result2, N, sizeof(uint32_t));

        for (uint32_t i = 0; i < N; i++) {
            assert(result1[i] == i * 5u + 7u);    // first dispatch: OLD value
            assert(result2[i] == i * 9u + 11u);   // second dispatch: NEW value
        }
        printf("graphics_tests: update_constant_buffer flush ordering (C1) passed\n");
        graphics::unset_texture_compute(2);
        graphics::release(&buf1); graphics::release(&buf2);
        graphics::release(&cb); graphics::release(&cs);
    }

    // --- Test 5: 2D uint storage texture clear + read into buffer. The 2D
    // clear kernels (g_clear2d_f / g_clear2d_u) were previously never
    // exercised by any test. ---
    {
        File f;
        char *code = load_shader("test_tex2d_uint_roundtrip.wgsl", &f);
        graphics::ComputeShader cs =
            graphics::get_compute_shader_from_code(code, (uint32_t)f.size);
        assert(graphics::is_ready(&cs));
        file_system::release_file(f);

        const uint32_t W = 8, H = 8;
        graphics::Texture2D tex =
            graphics::get_texture2D(nullptr, W, H, graphics::Format::R32_UINT);
        assert(graphics::is_ready(&tex));
        graphics::clear_texture_uint(&tex, 42u);

        graphics::StructuredBuffer buf = graphics::get_structured_buffer(sizeof(uint32_t), W * H);
        graphics::set_texture_compute(&tex, 0);
        graphics::set_structured_buffer(&buf, 1);
        graphics::set_compute_shader(&cs);
        graphics::run_compute(W / 8, H / 8, 1);
        graphics::unset_texture_compute(0);
        graphics::unset_texture_compute(1);

        uint32_t result[W * H];
        graphics::capture_structured_buffer(&buf, result, W * H, sizeof(uint32_t));
        for (uint32_t i = 0; i < W * H; i++)
            assert(result[i] == 42u);
        printf("graphics_tests: tex2d uint clear+roundtrip passed\n");
        graphics::release(&buf); graphics::release(&tex); graphics::release(&cs);
    }

    // --- Test 6: group-0 binding driven by shader declaration, not shadow state. ---
    {
        File ids_file;
        char *ids_code = load_shader("test_write_ids.wgsl", &ids_file);
        graphics::ComputeShader ids_shader =
            graphics::get_compute_shader_from_code(ids_code, (uint32_t)ids_file.size);
        assert(graphics::is_ready(&ids_shader));
        file_system::release_file(ids_file);

        File tex_file;
        char *tex_code = load_shader("test_tex3d_roundtrip.wgsl", &tex_file);
        graphics::ComputeShader tex_shader =
            graphics::get_compute_shader_from_code(tex_code, (uint32_t)tex_file.size);
        assert(graphics::is_ready(&tex_shader));
        file_system::release_file(tex_file);

        graphics::ConstantBuffer cfg_buffer = graphics::get_constant_buffer(16);
        uint32_t cfg[4] = {5, 7, 0, 0};
        graphics::update_constant_buffer(&cfg_buffer, cfg);

        const uint32_t W = 8, H = 8, D = 8;
        graphics::Texture3D tex3d =
            graphics::get_texture3D(nullptr, W, H, D, graphics::Format::R32_FLOAT);
        graphics::clear_texture(&tex3d, 2.5f);

        graphics::StructuredBuffer out_buffer =
            graphics::get_structured_buffer(sizeof(uint32_t), 1000);

        // (a) Uniform still bound in shadow state from an earlier test is
        //     harmless for a shader that declares no @group(0).
        graphics::set_constant_buffer(&cfg_buffer, 0);      // deliberately stale
        graphics::set_compute_shader(&tex_shader);          // test_tex3d_roundtrip: no @group(0)
        assert(!tex_shader.uses_group0);
        graphics::StructuredBuffer readback_buffer =
            graphics::get_structured_buffer(sizeof(float), W * H * D);
        graphics::set_texture_compute(&tex3d, 0);
        graphics::set_structured_buffer(&readback_buffer, 1);
        graphics::run_compute(W / 4, H / 4, D / 4);        // must NOT Dawn-error (M2a needed a manual reset here)
        float readback_values[W * H * D];
        graphics::capture_structured_buffer(&readback_buffer, readback_values, W * H * D, sizeof(float));
        for (uint32_t i = 0; i < W * H * D; i++)
            assert(readback_values[i] == 2.5f);  // texture was cleared to 2.5f; this proves dispatch worked
        graphics::unset_texture_compute(0);
        graphics::unset_structured_buffer(1);
        graphics::release(&readback_buffer);

        // (b) uses_group0 detection on a uniform-declaring shader.
        assert(ids_shader.uses_group0);

        // (c) unset_structured_buffer clears a slot (rebind + dispatch still valid).
        graphics::set_compute_shader(&ids_shader);
        graphics::set_constant_buffer(&cfg_buffer, 0);
        graphics::set_structured_buffer(&out_buffer, 2);
        graphics::unset_structured_buffer(2);
        graphics::set_structured_buffer(&out_buffer, 2);
        graphics::run_compute((1000 + 63) / 64, 1, 1);     // ids_shader has @workgroup_size(64)
        uint32_t ids_result[1000];
        graphics::capture_structured_buffer(&out_buffer, ids_result, 1000, sizeof(uint32_t));
        for (uint32_t i = 0; i < 1000; i++)
            assert(ids_result[i] == i * 5u + 7u);  // cfg.mul=5, MULTIPLIER default=1, cfg.add=7
        graphics::unset_structured_buffer(2);

        printf("graphics_tests: group0 hardening + unset_structured_buffer passed\n");
        graphics::release(&tex3d); graphics::release(&out_buffer);
        graphics::release(&cfg_buffer); graphics::release(&ids_shader); graphics::release(&tex_shader);
    }

    graphics::release();
    printf("graphics_tests: all passed\n");
    return 0;
}
