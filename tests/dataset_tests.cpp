#include "../cpplib/dataset.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

// A CSV must load exactly like the packed pair tools/pack_catalog.py makes
// from it: same float32 rows, same bounds, mean weight to float rounding.
static void check_csv_matches_packed(const std::string &csv, const std::string &packed, double weight_scale)
{
    Dataset a, b;
    std::string err;
    bool ok = dataset::load(csv, weight_scale, &a, &err);
    if (!ok) printf("%s\n", err.c_str());
    assert(ok);
    ok = dataset::load(packed, 1.0f, &b, &err);
    if (!ok) printf("%s\n", err.c_str());
    assert(ok);
    assert(a.count == b.count);
    assert(a.xyzw == b.xyzw);   // bit-identical float32 rows
    for (int i = 0; i < 3; ++i) {
        assert(a.min[i] == b.min[i]);
        assert(a.max[i] == b.max[i]);
    }
    // numpy's float32 mean vs a double accumulator: equal to ~1e-6 relative.
    assert(std::fabs(a.mean_weight - b.mean_weight) <= 1e-5f * b.mean_weight);
    printf("  %s == %s (%d rows)\n", csv.c_str(), packed.c_str(), a.count);
}

static void write(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

int main()
{
    const std::string data = DATA_ROOT "/data/";

    // Real catalogs, all in 1e12 Msun once loaded. The 2MRS CSV is already
    // in 1e12 Msun, so it is packed at weight scale 1. The SDSS source CSV
    // (sample_3D_linW.csv) is in 1e9 Msun and is packed at 0.001 by
    // tools/pack_vac_catalog.py; sdss_vac_1e12Msun.csv is the same rows
    // already divided by 1000. The SDSS pair is gitignored; skip it when
    // absent.
    check_csv_matches_packed(data + "2MRS/2mrs_gui.csv", data + "2MRS/2mrs_gui", 1.0f);
    const std::string sdss = data + "SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0";
    if (dataset::exists(sdss)) {
        check_csv_matches_packed(data + "reference/sample_3D_linW.csv", sdss, 0.001);
        check_csv_matches_packed(data + "SDSS/sdss_vac_1e12Msun.csv", sdss, 1.0f);
    } else
        printf("  (skipped SDSS: %s.bin not packed)\n", sdss.c_str());

    // Either file of a packed pair names the pair.
    assert(dataset::packed_base("a/b_metadata.txt") == "a/b");
    assert(dataset::packed_base("a/b.bin") == "a/b");
    assert(dataset::packed_base("a/b") == "a/b");
    assert(dataset::exists(data + "2MRS/2mrs_gui.bin"));
    assert(dataset::exists(data + "2MRS/2mrs_gui_metadata.txt"));

    // Header, comments, blank lines, other separators, extra columns.
    write("dataset_test.csv",
          "x,y,z,weight\n"
          "# comment\n"
          "\n"
          "1,2,3,4\n"
          "-1;0.5;6;2,99\n"
          "  0\t-2\t1e1\t6\n");
    Dataset d;
    std::string err;
    assert(dataset::load("dataset_test.csv", 0.5f, &d, &err));
    assert(d.count == 3);
    assert(d.min[0] == -1 && d.max[0] == 1);
    assert(d.min[1] == -2 && d.max[1] == 2);
    assert(d.min[2] == 3 && d.max[2] == 10);
    assert(d.xyzw[3] == 2.0f && d.xyzw[7] == 1.0f && d.xyzw[11] == 3.0f);   // weights * 0.5
    assert(d.mean_weight == 2.0f);

    // Too few columns and non-positive weights fail with the line number.
    write("dataset_test.csv", "1,2,3\n");
    assert(!dataset::load("dataset_test.csv", 1.0f, &d, &err));
    assert(err.find(":1:") != std::string::npos);
    write("dataset_test.csv", "1,2,3,4\n1,2,3,0\n");
    assert(!dataset::load("dataset_test.csv", 1.0f, &d, &err));
    assert(err.find(":2:") != std::string::npos);
    write("dataset_test.csv", "x,y,z,w\n");
    assert(!dataset::load("dataset_test.csv", 1.0f, &d, &err));
    remove("dataset_test.csv");

    assert(!dataset::load("no_such_dataset", 1.0f, &d, &err));

    printf("dataset_tests: all passed\n");
    return 0;
}
