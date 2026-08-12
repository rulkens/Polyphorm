// Headless: every production WGSL shader must compile on the pinned Dawn.
// Catches the "uncertain lines" from translation-notes.md §7 at ctest time
// instead of at app startup.
#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
#include <cstdio>

static void check_compiles(const char *path, int expect_uses_group0)
{
    File f = file_system::read_file(path);
    assert(f.data != NULL);
    graphics::ComputeShader cs = graphics::get_compute_shader_from_code((char *)f.data, f.size);
    if (!graphics::is_ready(&cs)) {
        printf("FAILED to compile %s (see Dawn error above)\n", path);
        assert(false);
    }
    printf("compiled OK: %s (uses_group0=%d)\n", path, (int)cs.uses_group0);
    assert((int)cs.uses_group0 == expect_uses_group0);
    graphics::release(&cs);
    file_system::release_file(f);
}

// M3 Task 2: vs/ps variants for the split render draft (vs_2d.wgsl,
// ps_particles_color.wgsl). Neither declares a @group(0) uniform (V9 —
// vs_2d_ps_particles_color.wgsl has no cbuffer at all), so both are expected
// uses_group0=0, unlike the two compute shaders below.
static void check_compiles_vs(const char *path, int expect_uses_group0)
{
    File f = file_system::read_file(path);
    assert(f.data != NULL);
    graphics::VertexShader vs =
        graphics::get_vertex_shader_from_code((char *)f.data, f.size);
    if (!graphics::is_ready(&vs)) {
        printf("FAILED to compile %s (see Dawn error above)\n", path);
        assert(false);
    }
    printf("compiled OK: %s (uses_group0=%d)\n", path, (int)vs.uses_group0);
    assert((int)vs.uses_group0 == expect_uses_group0);
    graphics::release(&vs);
    file_system::release_file(f);
}

static void check_compiles_ps(const char *path, int expect_uses_group0)
{
    File f = file_system::read_file(path);
    assert(f.data != NULL);
    graphics::PixelShader ps =
        graphics::get_pixel_shader_from_code((char *)f.data, f.size);
    if (!graphics::is_ready(&ps)) {
        printf("FAILED to compile %s (see Dawn error above)\n", path);
        assert(false);
    }
    printf("compiled OK: %s (uses_group0=%d)\n", path, (int)ps.uses_group0);
    assert((int)ps.uses_group0 == expect_uses_group0);
    graphics::release(&ps);
    file_system::release_file(f);
}

int main()
{
    bool ok = graphics::init();   // headless: no init_swap_chain (matches graphics_tests.cpp)
    assert(ok);

    check_compiles(SHADER_DIR "/cs_agents_propagate.wgsl", 1);
    check_compiles(SHADER_DIR "/cs_field_decay.wgsl", 1);
    check_compiles(SHADER_DIR "/cs_density_histo.wgsl", 1);

    // M3 Task 2: particle-chain shaders.
    check_compiles(SHADER_DIR "/cs_particles_transform.wgsl", 1);
    check_compiles(SHADER_DIR "/cs_particles_blit.wgsl", 1);
    check_compiles_vs(SHADER_DIR "/vs_2d.wgsl", 0);
    check_compiles_ps(SHADER_DIR "/ps_particles_color.wgsl", 0);

    // M4b Task 3: vs_3d.wgsl declares the full RenderingConfig cbuffer at
    // @group(0) @binding(0), unlike vs_2d.wgsl (V9 — no cbuffer at all).
    check_compiles_vs(SHADER_DIR "/vs_3d.wgsl", 1);

    // M4b Task 4: ps_volume_trace.wgsl declares the full RenderingConfig
    // cbuffer at @group(0) @binding(0), same as vs_3d.wgsl.
    check_compiles_ps(SHADER_DIR "/ps_volume_trace.wgsl", 1);

    // M4b Task 6: ps_volume_highlight.wgsl / ps_volume_overdensity.wgsl,
    // same full RenderingConfig cbuffer shape as ps_volume_trace.wgsl.
    check_compiles_ps(SHADER_DIR "/ps_volume_highlight.wgsl", 1);
    check_compiles_ps(SHADER_DIR "/ps_volume_overdensity.wgsl", 1);

    graphics::release();
    printf("All shader compile tests passed\n");
    return 0;
}
