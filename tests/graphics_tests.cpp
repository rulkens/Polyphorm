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

    graphics::release();
    printf("graphics_tests: all passed\n");
    return 0;
}
