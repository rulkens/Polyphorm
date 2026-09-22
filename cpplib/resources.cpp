#include "resources.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <mach-o/dyld.h>

namespace fs = std::filesystem;

namespace
{
    bool g_bundle = false;
    std::string g_shader_root = SHADER_ROOT;
    std::string g_data_root = DATA_ROOT;
    std::string g_output_root; // empty = working directory (dev mode)
    std::string g_dataset_storage;

    fs::path executable_dir()
    {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buf(size, '\0');
        if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
        std::error_code ec;
        fs::path exe = fs::canonical(buf.c_str(), ec);
        return ec ? fs::path() : exe.parent_path();
    }
}

void resources::init(const char **dataset_path)
{
    std::error_code ec;
    fs::path res = executable_dir() / ".." / "Resources";
    if (!fs::exists(res / "shaders" / "cs_agents_propagate.wgsl", ec)) return;
    res = fs::canonical(res, ec);
    if (ec) return;

    g_bundle = true;
    g_shader_root = (res / "shaders").string();
    g_data_root = res.string();
    const char *home = getenv("HOME");
    // ~/Polyphorm, not ~/Documents/Polyphorm: Documents is TCC-protected,
    // and an ad-hoc signed app gets a new code identity on every rebuild,
    // so a granted Documents permission silently stops applying and every
    // write fails with EPERM.
    g_output_root = (fs::path(home ? home : "/tmp") / "Polyphorm").string();

    // A --dataset given relative to where the user launched from must
    // survive the chdir below; bundled "data/..." paths resolve against
    // Resources instead.
    if (dataset_path && *dataset_path) {
        fs::path p(*dataset_path);
        fs::path meta = p; meta += "_metadata.txt";
        if (p.is_relative() && fs::exists(meta, ec)) {
            g_dataset_storage = fs::absolute(p, ec).string();
            *dataset_path = g_dataset_storage.c_str();
        }
    }

    if (chdir(g_data_root.c_str()) != 0)
        fprintf(stderr, "warning: could not chdir to %s\n", g_data_root.c_str());
    printf("-> bundle resources: %s\n-> output folder: %s\n",
           g_data_root.c_str(), g_output_root.c_str());
}

bool resources::is_bundle() { return g_bundle; }

std::string resources::shader(const char *name) { return g_shader_root + "/" + name; }

std::string resources::data(const char *rel_path) { return g_data_root + "/" + rel_path; }

std::string resources::output(const char *rel_path)
{
    if (g_output_root.empty()) return rel_path;
    // Make the parent folder too: upstream relied on bin/export/ and
    // bin/capture/ already existing, which a fresh ~/Polyphorm
    // does not have.
    fs::path p = fs::path(g_output_root) / rel_path;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::string out = p.string();
    // Keep the trailing slash for "export/"-style prefixes.
    if (rel_path[0] && rel_path[strlen(rel_path) - 1] == '/' && out.back() != '/') out += '/';
    return out;
}
