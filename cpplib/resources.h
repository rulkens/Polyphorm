#pragma once
#include <string>

// Runtime resource lookup, so the same binary works both from the source
// tree (dev) and from inside a relocatable Polyphorm.app bundle (release).
//
// Dev mode (binary not inside a bundle): shaders and palettes come from the
// compile-time SHADER_ROOT / DATA_ROOT absolute paths, the working directory
// is left alone (run_*.sh cd into bin/), and outputs go to the working
// directory exactly as before.
//
// Bundle mode (<exe>/../Resources/shaders exists): shaders, palettes,
// config.polyp and data/ are read from Contents/Resources, and the process
// chdirs there so the upstream cwd-relative reads keep working. Everything
// the app writes (export/, capture/, imgui.ini) goes to
// $POLYPHORM_OUTPUT_DIR (set by the launcher from the user's first-launch
// choice) or ~/Polyphorm — never into the (signed, possibly read-only
// translocated) bundle, and not ~/Documents by default, which is
// privacy-protected (TCC).
namespace resources
{
    // Call first thing in main(). May rewrite *dataset_path to an absolute
    // path when it is relative to the launch directory (bundle mode chdirs).
    void init(const char **dataset_path);

    bool is_bundle();
    std::string shader(const char *name);     // <shader root>/<name>
    std::string data(const char *rel_path);   // <data root>/<rel_path>
    std::string output(const char *rel_path); // where writes go; creates the output root
}
