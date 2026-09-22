#include "dataset.h"

#include <cctype>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

namespace
{
    bool ends_with(const std::string &s, const char *suffix)
    {
        size_t n = strlen(suffix);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
        return true;
    }

    bool is_file(const std::string &path)
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }

    bool load_csv(const std::string &path, double weight_scale, Dataset *out, std::string *error)
    {
        std::ifstream in(path);
        if (!in.good()) { *error = "cannot open " + path; return false; }

        std::string line;
        int line_no = 0, skipped = 0;
        double weight_sum = 0.0;
        while (std::getline(in, line)) {
            ++line_no;
            const char *p = line.c_str();
            while (*p == ' ' || *p == '\t') ++p;
            // Header rows, comments and blank lines: anything not starting
            // like a number.
            if (!(isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) { ++skipped; continue; }

            double v[4];
            int n = 0;
            while (n < 4) {
                char *end;
                v[n] = strtod(p, &end);
                if (end == p) break;
                ++n;
                p = end;
                while (*p == ' ' || *p == '\t') ++p;
                if (*p == ',' || *p == ';') ++p;
            }
            if (n < 4) {
                *error = path + ":" + std::to_string(line_no) + ": expected 4 numeric columns (x,y,z,weight), found " + std::to_string(n);
                return false;
            }
            float x = (float)v[0], y = (float)v[1], z = (float)v[2];
            float w = (float)(v[3] * weight_scale);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !(w > 0.0f) || !std::isfinite(w)) {
                *error = path + ":" + std::to_string(line_no) + ": coordinates must be finite and the weight positive";
                return false;
            }
            out->xyzw.push_back(x); out->xyzw.push_back(y); out->xyzw.push_back(z); out->xyzw.push_back(w);
            const float c[3] = {x, y, z};
            for (int a = 0; a < 3; ++a) {
                if (out->count == 0 || c[a] < out->min[a]) out->min[a] = c[a];
                if (out->count == 0 || c[a] > out->max[a]) out->max[a] = c[a];
            }
            weight_sum += w;
            ++out->count;
        }
        if (out->count == 0) { *error = path + ": no data rows"; return false; }
        out->mean_weight = (float)(weight_sum / out->count);
        if (skipped) printf("-> csv: skipped %d non-data line(s)\n", skipped);
        return true;
    }

    bool load_packed(const std::string &base, Dataset *out, std::string *error)
    {
        // Same positional parse as upstream main.cpp: skip to each '=' and
        // read the value, in file order.
        std::ifstream meta(base + "_metadata.txt");
        if (!meta.good()) { *error = "missing " + base + "_metadata.txt"; return false; }
        std::string key;
        float *fields[7] = {&out->min[0], &out->max[0], &out->min[1], &out->max[1], &out->min[2], &out->max[2], &out->mean_weight};
        std::getline(meta, key, '='); meta >> out->count;
        for (float *f : fields) { std::getline(meta, key, '='); meta >> *f; }
        if (meta.fail() || out->count <= 0) { *error = "malformed " + base + "_metadata.txt"; return false; }

        FILE *fp = fopen((base + ".bin").c_str(), "rb");
        if (!fp) { *error = "missing " + base + ".bin"; return false; }
        out->xyzw.resize((size_t)out->count * 4);
        size_t got = fread(out->xyzw.data(), sizeof(float) * 4, (size_t)out->count, fp);
        fclose(fp);
        if (got != (size_t)out->count) {
            *error = base + ".bin holds " + std::to_string(got) + " rows, metadata says " + std::to_string(out->count);
            return false;
        }
        return true;
    }
}

bool dataset::is_csv(const std::string &path) { return ends_with(path, ".csv"); }

std::string dataset::packed_base(const std::string &path)
{
    if (ends_with(path, "_metadata.txt")) return path.substr(0, path.size() - strlen("_metadata.txt"));
    if (ends_with(path, ".bin")) return path.substr(0, path.size() - strlen(".bin"));
    return path;
}

bool dataset::exists(const std::string &path)
{
    if (is_csv(path)) return is_file(path);
    std::string base = packed_base(path);
    return is_file(base + ".bin") && is_file(base + "_metadata.txt");
}

bool dataset::load(const std::string &path, double weight_scale, Dataset *out, std::string *error)
{
    *out = Dataset();
    return is_csv(path) ? load_csv(path, weight_scale, out, error)
                        : load_packed(packed_base(path), out, error);
}
