// Headless: every production WGSL shader must compile on the pinned Dawn.
// Catches the "uncertain lines" from translation-notes.md §7 at ctest time
// instead of at app startup.
#include "../cpplib/graphics.h"
#include "../cpplib/file_system.h"
#include <cassert>
#include <cstdio>

static void check_compiles(const char *path)
{
    File f = file_system::read_file(path);
    assert(f.data != NULL);
    graphics::ComputeShader cs = graphics::get_compute_shader_from_code((char *)f.data, f.size);
    if (!graphics::is_ready(&cs)) {
        printf("FAILED to compile %s (see Dawn error above)\n", path);
        assert(false);
    }
    printf("compiled OK: %s (uses_group0=%d)\n", path, (int)cs.uses_group0);
    graphics::release(&cs);
    file_system::release_file(f);
}

int main()
{
    bool ok = graphics::init();   // headless: no init_swap_chain (matches graphics_tests.cpp)
    assert(ok);

    check_compiles(SHADER_DIR "/cs_agents_propagate.wgsl");
    check_compiles(SHADER_DIR "/cs_field_decay.wgsl");
    check_compiles(SHADER_DIR "/cs_density_histo.wgsl");

    graphics::release();
    printf("All shader compile tests passed\n");
    return 0;
}
