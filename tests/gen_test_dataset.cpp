// M2b energy_smoke fixture: writes a small synthetic point catalog into
// argv[1], in the exact format main.cpp's config/metadata/binary loaders
// expect (4 floats per point: x y z mass). Three Gaussian blobs on a
// diagonal give agents enough structure to converge on within a few
// hundred iterations.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

static unsigned s = 12345u;
static float frand() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) / 16777216.0f; }
static float gauss() { return (frand() + frand() + frand() + frand() - 2.0f) * 0.8f; }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: gen_test_dataset <dir>\n"); return 2; }
    std::string dir(argv[1]);
    const int N = 400;
    const char *name = "testdata";

    FILE *cfg = fopen((dir + "/config.polyp").c_str(), "w");
    if (!cfg) { printf("cannot write to %s\n", dir.c_str()); return 2; }
    fprintf(cfg, "NUM_AGENTS=100000\nGRID_RESOLUTION=64\nGRID_PADDING=0.25\nSCREEN_X=640\nSCREEN_Y=480\nCAMERA_FOV=45\nHISTOGRAM_BASE=2.0\n");
    fclose(cfg);

    float centers[3][3] = {{-30, -30, -30}, {0, 0, 0}, {30, 30, 30}};
    FILE *bin = fopen((dir + "/" + name + ".bin").c_str(), "wb");
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    double mass_sum = 0.0;
    for (int i = 0; i < N; ++i) {
        float *c = centers[i % 3];
        float p[4] = { c[0] + 8.0f * gauss(), c[1] + 8.0f * gauss(), c[2] + 8.0f * gauss(),
                       1.0f + 99.0f * frand() };  // mass
        for (int k = 0; k < 3; ++k) { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
        mass_sum += p[3];
        fwrite(p, sizeof(float), 4, bin);
    }
    fclose(bin);

    // mean_weight matches main.cpp's log10(1+mass) weighting convention:
    // metadata's value is used as the normalizer, upstream files store the mean of the raw column.
    FILE *meta = fopen((dir + "/" + name + "_metadata.txt").c_str(), "w");
    fprintf(meta, "n=%d\nxmin=%f\nxmax=%f\nymin=%f\nymax=%f\nzmin=%f\nzmax=%f\nmean_weight=%f\n",
            N, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], (float)(mass_sum / N));
    fclose(meta);
    printf("wrote %d points to %s\n", N, dir.c_str());
    return 0;
}
