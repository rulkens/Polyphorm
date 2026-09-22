#pragma once
#include <string>
#include <vector>

// Input catalog loading. Two formats:
//
//  - The upstream packed pair <base>.bin (float32 x,y,z,weight rows) +
//    <base>_metadata.txt (positional keys: count, min/max X/Y/Z, mean
//    weight). The path may be the bare base, or either file of the pair.
//    Metadata values are used as written, exactly like upstream.
//
//  - A CSV of x,y,z,weight rows (comma, semicolon, tab or space separated;
//    extra columns ignored). Lines that do not start with a number — a
//    header row, '#' comments, blank lines — are skipped. The metadata is
//    computed the way tools/pack_catalog.py writes it (float32 bounds,
//    mean of the float32 weights), so a CSV loads like its packed pair.
//
// weight_scale multiplies CSV weights before anything else (the packer's
// --weight-scale); it matters because the simulation uses
// log10(1 + weight) / mean_weight. It is ignored for packed pairs, whose
// weights were already scaled when packed.
struct Dataset
{
    std::vector<float> xyzw;  // count * 4 floats
    int count = 0;
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    float mean_weight = 0.0f;
};

namespace dataset
{
    bool is_csv(const std::string &path);
    // Strip a trailing ".bin" or "_metadata.txt" to get a packed pair's base.
    std::string packed_base(const std::string &path);
    // True when path names something load() can open.
    bool exists(const std::string &path);
    bool load(const std::string &path, double weight_scale, Dataset *out, std::string *error);
}
