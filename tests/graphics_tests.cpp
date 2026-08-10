#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
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
        // DEVIATION from brief: test_tex3d_roundtrip.wgsl declares no @group(0)
        // uniform, but Test 1's set_constant_buffer(&cb, 0) left g_uniform_buffer
        // shadow state set. run_compute's group-0 code binds unconditionally on
        // "g_uniform_buffer is set" rather than "the shader declares group 0",
        // so without this reset it tries to bind 1 entry against pipeline B's
        // auto-generated *empty* group-0 layout -> Dawn validation error
        // ("binding index 0 not present in the bind group layout"), silently
        // invalidating the whole compute pass. Reset via the existing public
        // API (an empty ConstantBuffer has a null wgpu::Buffer, so the
        // `if (g_uniform_buffer)` guard in run_compute skips group 0 entirely)
        // rather than adding a new unset_constant_buffer to the frozen header.
        graphics::ConstantBuffer no_uniform = {};
        graphics::set_constant_buffer(&no_uniform, 0);
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
        // Reset the group-0 uniform shadow state (this kernel declares no
        // @group(0) uniform); see the DEVIATION note in Test 2 above for why.
        graphics::ConstantBuffer no_uniform = {};
        graphics::set_constant_buffer(&no_uniform, 0);
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

    graphics::release();
    printf("graphics_tests: all passed\n");
    return 0;
}
