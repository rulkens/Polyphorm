# M5: Export + VAC Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement F6's `save_texture3D` (byte-format-exact f16 `.bin` export), pack the VAC-scale input catalog, and produce the first honest d8 log-trace Pearson measurement of the port against the published SDSS DR17 Cosmic Slime VAC — pipeline + numbers, no pass/fail gate (measure-first, human sets the bar).

**Architecture:** Five components in dependency order (design §2): a task-zero memory feasibility check; `save_texture3D` in `cpplib/graphics.cpp` (3D GPU readback → CPU f32→f16 → tight Z-major pack); a `--export` headless CLI flag arming the existing quirk-preserved F6 block; a catalog converter `tools/pack_vac_catalog.py` with a read-only grid predictor (`--verify-grid`); and an offline comparison pipeline `tools/validate/compare_trace.py` reporting eight Pearson numbers at the d8 tier. M5a (Tasks 2–4) is pure C++ testable on the shipped demo slice; M5b (Tasks 5–11) is Python + one long headless GPU run.

**Tech Stack:** C++17, Dawn/WebGPU (Metal backend), CMake + CTest; Python 3 offline-only (numpy, scikit-image, matplotlib).

**Requirements source (authoritative, do not relitigate):**
`docs/superpowers/research/m5/m5-export-validation-design.md`, supported by
`docs/superpowers/research/m5/export-path-research.md` and
`docs/superpowers/research/m5/validation-target-research.md`.

## Global Constraints

- NEVER delete or wipe build/ (contains a ~1.4GB Dawn checkout). Incremental builds only: `cmake -B build`, then `nice -n 19 cmake --build build -j 8` (never bare -j).
- All 7 ctest suites green before EVERY commit (cpplib_tests, file_system_tests, graphics_tests, shader_compile_tests, render_path_tests, sim_kernel_tests, energy_smoke), run as `cd build && ctest --output-on-failure`.
- Every commit message ends with trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- Quirk fidelity: bug-for-bug preservation of upstream behavior; innocent "fixes" of upstream oddities are defects. File formats count: save_texture3D output must be byte-format-identical to upstream's (headerless raw, Z-major/X-fastest, f16). Mark preserved oddities with QUIRK() comments per the established convention.
- Never launch the GUI app in automated tasks; never use osascript or any macOS accessibility automation. Long sim runs use headless mode.
- Python tooling: offline scripts only (numpy/scipy/skimage), no network access; mirror conventions from ~/Development/vendor/python/PolyPhy/rhizome/tools/compareCubes.py where the design says to.
- The PolyPhy CSV source for the catalog converter is ~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv (read-only — never modify anything in that repo).
- Long-running validation sims must be launched `run_in_background` with output to a log file, never blocking a task's test cycle; task deliverables end at "run launched + early iterations sane", with analysis as a separate task.

**Additional binding decisions from the design (final, 2026-08-12):**

- Full-catalog validation: the shipped 37k slice stays untouched as the visual/demo dataset; validation uses the packed 324,901-pt catalog.
- Measure-first success bar: **the plan encodes NO numeric pass/fail threshold anywhere.** The top-level spec's "≥0.9 at d8" is superseded for this milestone. The final task presents numbers + PNG to the human, who sets the bar. Quirk A/B hunts are post-M5.

## Shared reference values (cited by multiple tasks)

| Item | Value |
|---|---|
| Reference d8 cube | `~/Development/js/skymap/data/raw/mcpm/mcpm_sdss_d8.npy` — float32, shape (89, 150, 91) = (X, Y, Z) |
| Reference metadata | `~/Development/js/skymap/data/raw/mcpm/export_metadata.txt` |
| VAC grid | 712 × 1200 × 728 vox; 556.288 × 937.564 × 568.789 Mpc; center (-239.469, -16.5618, 201.275) Mpc; 0.78131 Mpc isotropic voxel |
| VAC sim params | 10M agents, sense 4.6 Mpc, move 0.1 Mpc, sense spread 20°, move spread 10°, persistence 0.8, agent deposit 0, sampling sharpness 2.5 |
| Input CSV | `~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv` — 324,901 rows, no header, `x,y,z,weight`, xyz Mpc, weight 10⁹ M☉ (= 1000× Polyphorm's 10¹² M☉ convention) |
| Shipped-catalog mean weight | 0.013950215 (`bin/data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0_metadata.txt`) |
| Packed dataset path | `bin/data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0{.bin,_metadata.txt}` |
| Native export size | 712·1200·728·2 = 1,243,929,600 bytes per `.bin` (trace and deposit each) |
| Half-res fallback | `GRID_RESOLUTION = 600` → predicted 356×600×364; d8 factors 356/89 = 600/150 = 364/91 = 4 |
| PolyPhy precedent | ~0.085 3D / ~0.37–0.41 axis-projection Pearson (linear min-max) vs this same reference, after 11 calibration runs with the correct input (risk context only, NOT a target) |

## File Structure

- Create: `docs/superpowers/research/m5/m5-run-log.md` — running record (task-zero verdict, probe results, run provenance, scan table, first measurement).
- Modify: `cpplib/graphics.h` — declare `f32_to_f16`; update `save_texture3D` comment.
- Modify: `cpplib/graphics.cpp` — add `#include <fstream>`, `f32_to_f16`, real `save_texture3D` (replace stub at 484–486).
- Modify: `tests/graphics_tests.cpp` — Test 0 (f16 vectors), Test 7 (save_texture3D round-trip).
- Modify: `main.cpp` — `--export` flag (arg loop ~355–358, final-frame arming ~1064), dataset-line fix (~1189), "SDSS VAC" constants block (~62–69).
- Modify: `bin/config.polyp` — `Grid resolution = 1200` (padding per back-solve).
- Modify: `bin/export/.gitignore`, `bin/data/SDSS/.gitignore` — ignore generated artifacts.
- Create: `tools/pack_vac_catalog.py` — CSV → `.bin`/`_metadata.txt` packer + `--verify-grid` predictor.
- Create: `tools/validate/compare_trace.py` — d8 comparison pipeline with `--self-test` and `--orientation-scan`.
- Create: `docs/superpowers/research/m5/run1-headless.log` — captured run output/E-series (committed after the run).
- Create: `docs/superpowers/research/m5/first-measurement/` — `report.json`, `report.txt`, `projections.png`.

**Out of scope (design §9/§10 — do not touch):** OpenVDB converter; F5 agents export (already works); `save_texture2D_HDR` / `capture_current_frame` stubs; HALO_COLOR_ANALYSIS / VELOCITY_ANALYSIS paths; blend/palette/HUD/DPI carryovers; d2/d4 tiers; setting the acceptance bar or quirk A/B hunts.

---

### Task 1: Task zero — memory feasibility verdict + M5 run log

**Files:**
- Create: `docs/superpowers/research/m5/m5-run-log.md`

**Interfaces:**
- Consumes: nothing (procedure only; design §6.2).
- Produces: the run-log document later tasks append to; the provisional `GRID_RESOLUTION` verdict (1200 native or 600 fallback) Task 6 writes into `bin/config.polyp`.

- [ ] **Step 1: Record the machine's unified-memory budget**

Run: `sysctl hw.memsize`
Expected: one line, e.g. `hw.memsize: <bytes>`. Note the value in GB (bytes / 2³⁰).

- [ ] **Step 2: Write the run log with the verdict**

Create `docs/superpowers/research/m5/m5-run-log.md` with this content, substituting the measured value from Step 1 for the two `<measured ...>` fields and choosing the verdict per the stated rule (these are runtime measurements, not design content — everything else is written as-is):

```markdown
# M5 run log — export + VAC validation

Working record for M5. Design: ../m5-export-validation-design.md (same dir).
Each task appends its section; nothing here is ever rewritten, only appended.

## Task zero: memory feasibility (design §6.2)

- `sysctl hw.memsize`: <measured bytes> (<measured GB> GB)
- Budget at native grid 712x1200x728 (~622M voxels):
  - 3 x r32float 3D textures (trail A/B + trace): ~7.5 GB
  - particle SoA (~10.3M x 6 f32): ~0.25 GB
  - export transient (padded readback ~2.5 GB mapped + ~1.2 GB f16 pack,
    sequential per texture): ~3.7 GB peak
  - rule of thumb (design §6.2): need >= 16 GB unified memory for the
    native-grid run + export headroom
- Provisional verdict: GRID_RESOLUTION = 1200 (native) if hw.memsize >= 16 GB,
  else 600 (half-res fallback; d8 comparison is unaffected — factors become 4,
  simulation fidelity is what the fallback costs, and the final report must
  state which resolution produced the numbers).
- Chosen: GRID_RESOLUTION = <1200 or 600, per the rule above>
- Empirical confirmation: deferred to the Task 7 allocation probe
  (`--headless` at full scale on the packed catalog; Dawn error callbacks
  name a failing limit fatally at startup, so it fails loudly in seconds
  if the verdict is wrong). 3D-texture dimension limit is not a concern:
  1200 < Metal's 2048.
```

- [ ] **Step 3: Run the full test suite (required before every commit)**

Run: `cd build && ctest --output-on-failure`
Expected: all 7 suites pass (cpplib_tests, file_system_tests, graphics_tests, shader_compile_tests, render_path_tests, sim_kernel_tests, energy_smoke).

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/research/m5/m5-run-log.md
git commit -m "docs: m5 task zero — memory feasibility verdict

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `f32_to_f16` helper + known-vector unit test

**Files:**
- Modify: `cpplib/graphics.h` (declaration, after the `save_texture3D` stub line ~165)
- Modify: `cpplib/graphics.cpp` (implementation, immediately above `save_texture3D` at ~484)
- Test: `tests/graphics_tests.cpp` (new Test 0, pure CPU, before `graphics::init()`)

**Interfaces:**
- Consumes: nothing.
- Produces: `uint16_t graphics::f32_to_f16(float v)` — IEEE 754 binary16 bit pattern, round-to-nearest-even. Task 3's `save_texture3D` calls it per texel; Task 3's round-trip test uses it as the conversion oracle.

- [ ] **Step 1: Write the failing test**

In `tests/graphics_tests.cpp`, add `#include <cmath>` to the includes at the top, and insert this block as the first statement inside `main()` (before `bool ok = graphics::init();` — the helper is pure CPU, no device needed):

```cpp
    // --- Test 0 (M5): f32->f16 export conversion, known IEEE 754 vectors.
    // Spec "Testing" requires a CTest unit for the f32->f16 export conversion;
    // vectors cover zero/sign, normals, f16 max, overflow->inf, smallest
    // normal, subnormal, and the round-to-nearest-even tie cases. ---
    {
        struct { float in; uint16_t expect; } vec[] = {
            {0.0f, 0x0000}, {-0.0f, 0x8000},
            {1.0f, 0x3C00}, {-2.0f, 0xC000}, {0.5f, 0x3800},
            {65504.0f, 0x7BFF},                  // f16 max finite
            {65536.0f, 0x7C00},                  // overflow -> +inf
            {-65536.0f, 0xFC00},                 // overflow -> -inf
            {INFINITY, 0x7C00}, {-INFINITY, 0xFC00},
            {6.103515625e-5f, 0x0400},           // smallest normal (2^-14)
            {5.9604644775390625e-8f, 0x0001},    // smallest subnormal (2^-24)
            {1.0009765625f, 0x3C01},             // 1 + 2^-10: exactly representable
            {1.00048828125f, 0x3C00},            // 1 + 2^-11: tie -> even (down)
            {1.00146484375f, 0x3C02},            // 1 + 3*2^-11: tie -> even (up)
        };
        for (auto &t : vec) {
            uint16_t got = graphics::f32_to_f16(t.in);
            if (got != t.expect) {
                fprintf(stderr, "f32_to_f16(%g) = 0x%04X, want 0x%04X\n",
                        (double)t.in, got, t.expect);
                assert(false);
            }
        }
        uint16_t nan_bits = graphics::f32_to_f16(NAN);
        assert((nan_bits & 0x7C00) == 0x7C00 && (nan_bits & 0x03FF) != 0); // any f16 NaN
        printf("graphics_tests: f32_to_f16 known vectors passed\n");
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `nice -n 19 cmake --build build -j 8 --target graphics_tests`
Expected: FAIL to compile — `no member named 'f32_to_f16' in namespace 'graphics'`.

- [ ] **Step 3: Declare and implement the helper**

In `cpplib/graphics.h`, change:

```cpp
void save_texture3D(Texture3D *texture, std::string filename);  // M2a: stub (M5)
```

to:

```cpp
void save_texture3D(Texture3D *texture, std::string filename);  // M2a: stub (M5)
// M5: f32 -> IEEE 754 binary16 bit pattern (round-to-nearest-even; hardware
// _Float16 cast on Apple clang). Public so the export-conversion CTest unit
// can drive it with known vectors (top-level spec, "Testing").
uint16_t f32_to_f16(float v);
```

In `cpplib/graphics.cpp`, insert immediately above `void save_texture3D(...)` (line ~484):

```cpp
uint16_t f32_to_f16(float v) {
    // Apple clang's _Float16 is hardware IEEE 754 binary16 with
    // round-to-nearest-even — the same rounding upstream's R16F texture
    // applied on every GPU store. The port stores r32float and converts once
    // at export instead (QUIRK(r16f_channel_truncation), main.cpp), so
    // quantization matches upstream's pipeline stage-for-stage in rounding
    // behavior. Inf/NaN/subnormals are defined by the _Float16 conversion;
    // nothing handled specially (design §3.4).
    _Float16 h = (_Float16)v;
    uint16_t bits;
    memcpy(&bits, &h, sizeof(bits));
    return bits;
}
```

(`<cstring>` is already included at the top of graphics.cpp — `memcpy` is used elsewhere in the file.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `nice -n 19 cmake --build build -j 8 --target graphics_tests && cd build && ctest -R graphics_tests --output-on-failure`
Expected: PASS, output contains `f32_to_f16 known vectors passed`.

- [ ] **Step 5: Full suite + commit**

Run: `cd build && ctest --output-on-failure` — all 7 suites green.

```bash
git add cpplib/graphics.h cpplib/graphics.cpp tests/graphics_tests.cpp
git commit -m "feat: f32->f16 export conversion helper + known-vector test

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: `save_texture3D` — 3D readback, f16 convert, Z-major pack

**Files:**
- Modify: `cpplib/graphics.cpp` (replace the stub at ~484–486; add `#include <fstream>`)
- Modify: `cpplib/graphics.h` (comment update only)
- Test: `tests/graphics_tests.cpp` (new Test 7, after Test 6, before `graphics::release()`)

**Interfaces:**
- Consumes: `uint16_t graphics::f32_to_f16(float v)` (Task 2); internal `ensure_encoder()` / `flush_commands()` / `wait_for(bool*)` statics already in graphics.cpp; `g_ctx.device`; `Texture3D{texture, width, height, depth, format}`.
- Produces: `void graphics::save_texture3D(Texture3D *texture, std::string filename)` — writes `filename + ".bin"`: headerless raw f16, tightly packed, `index = z*width*height + y*width + x` (Z-major/X-fastest), exactly `width*height*depth*2` bytes. Signature unchanged (2 call sites in main.cpp:1182/1184/1185 need no edits). No `.dds` sibling (dropped per spec). Task 4's F6 smoke and Task 9's run consume the on-disk format; Task 8's `load_export` pins the same layout in Python.

- [ ] **Step 1: Write the failing round-trip test**

In `tests/graphics_tests.cpp`, add `#include <vector>` to the includes, and insert this block after Test 6's closing brace and before `graphics::release();`:

```cpp
    // --- Test 7 (M5): save_texture3D byte-exact round-trip. W*4 = 280 B is
    // NOT 256-aligned, forcing the Dawn bytesPerRow de-padding path. Distinct
    // integer texel values (all < 2048, exact in f16) pin the Z-major/
    // X-fastest layout: any axis-order or de-pad bug scrambles the byte
    // comparison. f32_to_f16 is the conversion oracle (its own vectors are
    // Test 0), so this test isolates layout + file format, not rounding. ---
    {
        const uint32_t W = 70, H = 5, D = 3;
        std::vector<float> src(W * H * D);
        for (uint32_t z = 0; z < D; ++z)
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    src[(z * H + y) * W + x] = (float)((z * H + y) * W + x);
        graphics::Texture3D tex = graphics::get_texture3D(
            src.data(), W, H, D, graphics::Format::R32_FLOAT);
        assert(graphics::is_ready(&tex));

        remove("m5_export_test.bin");
        graphics::save_texture3D(&tex, "m5_export_test");   // appends ".bin"

        FILE *f = fopen("m5_export_test.bin", "rb");
        assert(f && "save_texture3D produced no .bin");
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        assert(size == (long)(W * H * D * 2));              // headerless raw f16
        fseek(f, 0, SEEK_SET);
        std::vector<uint16_t> got(W * H * D);
        size_t n = fread(got.data(), sizeof(uint16_t), got.size(), f);
        assert(n == got.size());
        fclose(f);
        for (size_t i = 0; i < got.size(); ++i)
            assert(got[i] == graphics::f32_to_f16(src[i])); // same Z-major order
        remove("m5_export_test.bin");
        printf("graphics_tests: save_texture3D round-trip passed\n");
        graphics::release(&tex);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `nice -n 19 cmake --build build -j 8 --target graphics_tests && cd build && ctest -R graphics_tests --output-on-failure`
Expected: FAIL — stderr shows `[graphics] save_texture3D: stub until M3/M5`, then the assert `save_texture3D produced no .bin` fires.

- [ ] **Step 3: Implement `save_texture3D`**

In `cpplib/graphics.cpp`: add `#include <fstream>` after `#include <vector>` at the top of the file. Then replace the stub:

```cpp
void save_texture3D(Texture3D *texture, std::string filename) {
    (void)texture; (void)filename; warn_once("save_texture3D");
}
```

with:

```cpp
void save_texture3D(Texture3D *texture, std::string filename) {
    // M5 (F6 export): GPU->CPU readback of the r32float 3D texture, CPU
    // f32->f16 conversion, single write of upstream's byte format — headerless
    // raw binary16, tightly packed, X fastest / Y next / Z slowest
    // (index = z*W*H + y*W + x; DirectXTex ScratchImage layout). The upstream
    // .dds sibling is dropped (port design decision). No endianness handling:
    // both platforms little-endian, upstream never byte-swapped. `filename`
    // arrives extension-less ("export/deposit", "export/trace"); ".bin" is
    // appended here. Like upstream, no directory is created — bin/export/
    // ships with the repo.
    if (texture->format != Format::R32_FLOAT) {
        // Only format the port's exportable textures use (main.cpp trail/trace
        // creation). Loud skip, not silent garbage (design §3.1).
        fprintf(stderr, "[graphics] save_texture3D: unsupported format, skipping %s\n",
                filename.c_str());
        return;
    }
    const uint32_t width = texture->width, height = texture->height,
                   depth = texture->depth;
    const uint32_t unpadded_bytes_per_row = width * 4;
    // Dawn's 256 B bytesPerRow rule for texture->buffer copies (same as
    // tests/render_path_tests.cpp readback helpers).
    const uint32_t padded_bytes_per_row = (unpadded_bytes_per_row + 255u) & ~255u;
    const uint64_t buffer_size = (uint64_t)padded_bytes_per_row * height * depth;

    // Transient readback buffer, created and destroyed per call — F6 is a
    // rare user action; no caching (design §3.2). ~2.5 GB at the native VAC
    // grid; device maxBufferSize is requested at the adapter maximum in
    // gpu_context.cpp precisely for this.
    wgpu::BufferDescriptor desc = {};
    desc.size = buffer_size;
    desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = g_ctx.device.CreateBuffer(&desc);

    // Copy -> flush -> MapAsync -> blocking ProcessEvents pump: the exact
    // capture_structured_buffer idiom. Synchronous same-frame stall is the
    // established, deliberate convention (D3D11 Map(READ) parity).
    ensure_encoder();
    wgpu::TexelCopyTextureInfo src = {};
    src.texture = texture->texture;
    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = readback;
    dst.layout.bytesPerRow = padded_bytes_per_row;
    dst.layout.rowsPerImage = height;
    wgpu::Extent3D extent = {width, height, depth};
    g_encoder.CopyTextureToBuffer(&src, &dst, &extent);
    flush_commands();

    bool done = false;
    readback.MapAsync(
        wgpu::MapMode::Read, 0, buffer_size, wgpu::CallbackMode::AllowProcessEvents,
        [&done](wgpu::MapAsyncStatus status, wgpu::StringView message) {
            if (status != wgpu::MapAsyncStatus::Success)
                fprintf(stderr, "[graphics] save_texture3D map failed: %.*s\n",
                        (int)message.length, message.data);
            done = true;
        });
    wait_for(&done);
    const uint8_t *mapped = (const uint8_t *)readback.GetConstMappedRange(0, buffer_size);
    if (!mapped) {
        fprintf(stderr, "[graphics] save_texture3D: no mapped data, skipping %s\n",
                filename.c_str());
        readback.Unmap();
        return;
    }

    // De-pad each row and convert f32->f16 while re-packing into the tight
    // Z-major output (design §3.3).
    std::vector<uint16_t> out((size_t)width * height * depth);
    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t y = 0; y < height; ++y) {
            const float *row = (const float *)(mapped
                + (uint64_t)z * padded_bytes_per_row * height
                + (uint64_t)y * padded_bytes_per_row);
            uint16_t *dst_row = out.data() + ((size_t)z * height + y) * width;
            for (uint32_t x = 0; x < width; ++x)
                dst_row[x] = f32_to_f16(row[x]);
        }
    }
    readback.Unmap();

    std::ofstream bin_file(filename + ".bin", std::ofstream::binary);
    if (!bin_file.good()) {
        // Port-side diagnostics only (upstream wrote unchecked); does not
        // alter the on-disk byte format.
        fprintf(stderr, "[graphics] save_texture3D: cannot open %s.bin\n",
                filename.c_str());
        return;
    }
    bin_file.write((const char *)out.data(),
                   (std::streamsize)(out.size() * sizeof(uint16_t)));
    bin_file.close();
}
```

Also update the declaration comment in `cpplib/graphics.h`: change the trailing comment on the `save_texture3D` line from `// M2a: stub (M5)` to `// M5: implemented (F6 f16 .bin export)`. (Do not touch the `save_texture2D_HDR` / `capture_current_frame` stub lines — out of M5 scope.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `nice -n 19 cmake --build build -j 8 --target graphics_tests && cd build && ctest -R graphics_tests --output-on-failure`
Expected: PASS, output contains `save_texture3D round-trip passed`.

- [ ] **Step 5: Full suite + commit**

Run: `cd build && ctest --output-on-failure` — all 7 suites green.

```bash
git add cpplib/graphics.h cpplib/graphics.cpp tests/graphics_tests.cpp
git commit -m "feat: save_texture3D — 3D readback, f16 convert, Z-major .bin export

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: `--export` headless flag + effective-dataset metadata line + F6 smoke

**Files:**
- Modify: `main.cpp` (arg loop ~353–358; final-frame branch ~1063–1065; metadata dataset line ~1189)
- Modify: `bin/export/.gitignore` (ignore generated export artifacts)

**Interfaces:**
- Consumes: `graphics::save_texture3D` (Task 3) via the untouched F6 block at main.cpp:1177–1226; existing `--headless N` / `--dataset <path>` flags; `bool store_deposit` (main.cpp:820); `std::string filename` (main.cpp:391).
- Produces: `--export` CLI flag — when set and headless, `store_deposit = true` on the final iteration so the existing export block runs on the last frame; `export_metadata.txt`'s `dataset:` line records the effective dataset. Tasks 7 and 9 invoke `--headless N --export --dataset <path>`.

- [ ] **Step 1: Add the flag and the arming site**

In `main.cpp`, replace:

```cpp
    int headless_frames = 0;   // 0 = windowed
    const char *dataset_override = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0 && i + 1 < argc) headless_frames = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) dataset_override = argv[i + 1];
    }
```

with:

```cpp
    int headless_frames = 0;   // 0 = windowed
    const char *dataset_override = NULL;
    // M5: --export arms F6's store_deposit on the final headless iteration —
    // headless mode has no keyboard input, and the validation run must not
    // launch the GUI. The quirk-preserved F6 block itself is untouched.
    bool export_on_exit = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0 && i + 1 < argc) headless_frames = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) dataset_override = argv[i + 1];
        if (strcmp(argv[i], "--export") == 0) export_on_exit = true;
    }
```

Then replace the final-iteration branch:

```cpp
      } else {
        if (simulation_config.n_iteration >= headless_frames) is_running = false;
      }
```

with:

```cpp
      } else {
        if (simulation_config.n_iteration >= headless_frames) {
            // M5: fire the (unchanged) F6 export block later this frame.
            if (export_on_exit) store_deposit = true;
            is_running = false;
        }
      }
```

- [ ] **Step 2: Fix the metadata dataset line**

In `main.cpp`, inside the `if (store_deposit)` block, replace:

```cpp
            metadata << "dataset: " << DATASET_NAME << std::endl;
```

with:

```cpp
            // M5: record the dataset actually loaded. Upstream wrote the
            // compile-time DATASET_NAME macro — which was its ONLY possible
            // source; with the port's --dataset override the macro can be
            // wrong. Writing the effective `filename` preserves upstream
            // *semantics* (record what was loaded), not upstream letters;
            // --dataset is port infrastructure, so this is not a quirk
            // violation (design §4).
            metadata << "dataset: " << filename << std::endl;
```

- [ ] **Step 3: Ignore generated export artifacts**

Append to `bin/export/.gitignore` (below the existing four directory lines):

```
# M5: generated headless-export artifacts (deposit/trace are ~1.2 GB each
# at the VAC grid — never committed; the run log records provenance instead)
/deposit.bin
/trace.bin
/export_metadata.txt
/halos_measurements.csv
/agents.txt
```

- [ ] **Step 4: Build**

Run: `cmake -B build && nice -n 19 cmake --build build -j 8`
Expected: clean incremental build.

- [ ] **Step 5: Headless F6 smoke on the shipped 37k slice (design §11 item 3)**

Run (from `bin/`, where `config.polyp` and `data/` live; grid resolution is still 1024 at this point):

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm/bin && \
../build/polyphorm --headless 50 --export \
  --dataset "data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0"
```

Expected: prints the grid line (dims in the ~560×1024×584 ballpark), `[headless] iteration ...` E lines, `Exporting simulation data...` / `Done exporting simulation data.`, exit 0 with `ENERGY RISING`. (If it exits 1 with `ENERGY NOT RISING` the export files are still written — verify them, and record the energy outcome as a finding.)

Then verify the export contract:

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm/bin && python3 - <<'EOF'
import numpy as np, re
meta = open('export/export_metadata.txt').read()
assert 'dataset: data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0' in meta, meta
x, y, z = map(int, re.search(r'grid resolution: (\d+) x (\d+) x (\d+)', meta).groups())
for name in ('trace', 'deposit'):
    a = np.fromfile(f'export/{name}.bin', np.float16)
    assert a.size == x * y * z, (name, a.size, x * y * z)   # size == X*Y*Z*2 bytes
    af = a.astype(np.float32)
    assert np.isfinite(af).all(), name                      # round-trips as finite f16
    assert af.max() > 0, name                               # sim actually deposited
print('F6 smoke OK:', x, y, z, '| trace max', float(np.fromfile("export/trace.bin", np.float16).astype(np.float32).max()))
EOF
```

Expected: `F6 smoke OK: ...` and `git status` shows no untracked files under `bin/export/`.

- [ ] **Step 6: Full suite + commit**

Run: `cd build && ctest --output-on-failure` — all 7 suites green.

```bash
git add main.cpp bin/export/.gitignore
git commit -m "feat: --export headless flag + effective-dataset metadata line

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Catalog converter — `tools/pack_vac_catalog.py` (pack mode)

**Files:**
- Create: `tools/pack_vac_catalog.py`
- Modify: `bin/data/SDSS/.gitignore`

**Interfaces:**
- Consumes: `~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv` (READ-ONLY — never modify anything in that repo).
- Produces: `bin/data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0.bin` (float32 XYZW records) + `..._metadata.txt` (positional key sequence `Number of points`, `Min X`, `Max X`, `Min Y`, `Max Y`, `Min Z`, `Max Z`, `Mean weight` — main.cpp's parser is positional, order matters). Python functions `load_catalog(csv_path) -> np.ndarray (N,4) float32` and `pack(data, out_base)` that Task 6 extends with `--verify-grid`.

- [ ] **Step 1: Write the script**

Create `tools/pack_vac_catalog.py`:

```python
#!/usr/bin/env python3
"""pack_vac_catalog.py — pack PolyPhy's bundled VAC input catalog into
Polyphorm's .bin/_metadata.txt input pair (M5 validation dataset).

Input : ~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv
        (READ-ONLY; 324,901 rows, no header, columns x,y,z,weight;
        xyz in Mpc, weight in 1e9 Msun — exactly 1000x Polyphorm's
        1e12 Msun bin convention, per rhizome DATA_LINEAGE.md)
Output: <out>.bin           float32 XYZW records (arr.tofile)
        <out>_metadata.txt  the exact positional key sequence
                            pack_data_celestial.py writes — main.cpp's
                            metadata parser is positional, order matters.

Offline only (numpy, no network). Mirrors pack_data_celestial.py's output
conventions; the shipped 37k demo slice is untouched.
"""
import argparse
import os
import sys

import numpy as np

CSV_DEFAULT = os.path.expanduser(
    '~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv')
# The VAC's own dataset name (reference export_metadata.txt) so the run's
# --dataset line matches the published metadata verbatim.
OUT_DEFAULT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'bin', 'data', 'SDSS',
    'sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0'))

EXPECTED_POINTS = 324901           # validation-target research §3
WEIGHT_DIVISOR = 1000.0            # 1e9 Msun (CSV) -> 1e12 Msun (Polyphorm)
SHIPPED_MEAN_WEIGHT = 0.013950215  # shipped galaxiesInSdssSlice metadata —
                                   # same sample family, so a 1000x unit slip
                                   # is unmistakable (design §5.3)


def load_catalog(csv_path):
    """(N, 4) float32 [x, y, z, weight_1e12Msun]; hard-fails on unit slips."""
    data = np.loadtxt(csv_path, delimiter=',')            # (N, 4) float64
    if data.ndim != 2 or data.shape[1] != 4:
        sys.exit(f'FATAL: expected 4 columns, got shape {data.shape}')
    if data.shape[0] != EXPECTED_POINTS:
        sys.exit(f'FATAL: expected {EXPECTED_POINTS} points, got {data.shape[0]}')
    data[:, 3] /= WEIGHT_DIVISOR
    if not (data[:, 3] > 0).all():
        sys.exit('FATAL: non-positive weights after conversion')
    mean_w = float(data[:, 3].mean())
    if not (SHIPPED_MEAN_WEIGHT / 10.0 < mean_w < SHIPPED_MEAN_WEIGHT * 10.0):
        sys.exit(f'FATAL: mean converted weight {mean_w} not within one order '
                 f'of magnitude of shipped {SHIPPED_MEAN_WEIGHT} — unit slip?')
    # float32 at the end, matching the upstream packer's np.float32 output.
    return data.astype(np.float32)


def pack(data, out_base):
    """Write <out_base>.bin + <out_base>_metadata.txt (positional keys)."""
    with open(out_base + '_metadata.txt', 'w') as f:
        f.write('Number of points = ' + str(data.shape[0]) + '\n')
        f.write('Min X = ' + str(np.min(data[:, 0])) + '\n')
        f.write('Max X = ' + str(np.max(data[:, 0])) + '\n')
        f.write('Min Y = ' + str(np.min(data[:, 1])) + '\n')
        f.write('Max Y = ' + str(np.max(data[:, 1])) + '\n')
        f.write('Min Z = ' + str(np.min(data[:, 2])) + '\n')
        f.write('Max Z = ' + str(np.max(data[:, 2])) + '\n')
        f.write('Mean weight = ' + str(np.mean(data[:, 3])) + '\n')
    data.tofile(out_base + '.bin')

    bbox_min = data[:, :3].min(axis=0)
    bbox_max = data[:, :3].max(axis=0)
    mid = 0.5 * (bbox_min + bbox_max)
    print(f'packed {data.shape[0]} points -> {out_base}.bin '
          f'({os.path.getsize(out_base + ".bin")} bytes)')
    print(f'bbox min {bbox_min}, max {bbox_max}')
    print(f'bbox midpoint {mid}  (VAC grid center: -239.469, -16.5618, 201.275)')
    print(f'mean weight {np.mean(data[:, 3])}  '
          f'(shipped-slice reference: {SHIPPED_MEAN_WEIGHT})')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--csv', default=CSV_DEFAULT)
    ap.add_argument('--out', default=OUT_DEFAULT,
                    help='output base path (no extension)')
    args = ap.parse_args()
    data = load_catalog(args.csv)
    pack(data, args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 2: Ignore the derived dataset**

Append to `bin/data/SDSS/.gitignore`:

```
/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0.bin
/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0_metadata.txt
```

(~5.2 MB of derived data stays out of git; the `t=10.3` sibling is already ignored there.)

- [ ] **Step 3: Run the packer and verify the outputs**

Run: `cd /Users/rulkens/Development/vendor/cpp/Polyphorm && python3 tools/pack_vac_catalog.py`
Expected: `packed 324901 points ...` with `.bin` size exactly 5,198,416 bytes (324901 × 4 × 4); printed bbox midpoint within ~1 Mpc of (-239.469, -16.5618, 201.275) on each axis (the strict sub-voxel assert is Task 6's `--verify-grid`); mean weight within one order of magnitude of 0.013950215 (hard-checked in-script).

Then round-trip and metadata checks:

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm && python3 - <<'EOF'
import numpy as np, os
base = 'bin/data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0'
assert os.path.getsize(base + '.bin') == 324901 * 16
arr = np.fromfile(base + '.bin', np.float32).reshape(-1, 4)
assert arr.shape == (324901, 4)
src = np.loadtxt(os.path.expanduser(
    '~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv'),
    delimiter=',')
assert np.allclose(arr[:, :3], src[:, :3].astype(np.float32))
assert np.allclose(arr[:, 3], (src[:, 3] / 1000.0).astype(np.float32))
lines = open(base + '_metadata.txt').read().splitlines()
keys = [l.split('=')[0].strip() for l in lines]
assert keys == ['Number of points', 'Min X', 'Max X', 'Min Y', 'Max Y',
                'Min Z', 'Max Z', 'Mean weight'], keys   # positional order
print('packer round-trip OK')
EOF
```

Expected: `packer round-trip OK`; `git status` shows no untracked files under `bin/data/SDSS/`.

- [ ] **Step 4: Full suite + commit**

Run: `cd build && ctest --output-on-failure` — all 7 suites green.

```bash
git add tools/pack_vac_catalog.py "bin/data/SDSS/.gitignore"
git commit -m "feat: VAC catalog packer (tools/pack_vac_catalog.py)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Grid predictor (`--verify-grid`) + run config (resolution 1200, SDSS VAC constants)

**Files:**
- Modify: `tools/pack_vac_catalog.py` (add predictor + `--verify-grid` mode)
- Modify: `bin/config.polyp` (Grid resolution 1024 → 1200; padding per back-solve)
- Modify: `main.cpp` (constants block, lines ~54–69)

**Interfaces:**
- Consumes: `load_catalog(csv_path)` from Task 5 (same file).
- Produces: `pack_vac_catalog.py --verify-grid [--resolution N] [--padding P]` — a read-only Python *predictor* of main.cpp:418–442's grid fit (advisory; the C++ stays authoritative, the run's own `export_metadata.txt` is ground truth — design §6.1); the run configuration Tasks 7/9 execute with. **The quirk-preserved C++ fit logic is NOT forked.**

- [ ] **Step 1: Add the predictor to `tools/pack_vac_catalog.py`**

Insert after the `SHIPPED_MEAN_WEIGHT` constant block:

```python
# Published VAC grid (skymap .../mcpm/export_metadata.txt) — the anchoring
# target. Strategy (design §6.1): do NOT fork main.cpp's quirk-preserved
# auto-fit; feed it the same inputs (this catalog's bbox + config knobs) and
# predict what it will produce. The C++ is authoritative; the run's own
# export_metadata.txt is the ground-truth check.
VAC_DIMS = (712, 1200, 728)
VAC_SIZE = (556.288, 937.564, 568.789)
VAC_CENTER = (-239.469, -16.5618, 201.275)
VAC_VOXEL = 0.78131  # Mpc, isotropic


def nearest_multiple_of(n, m):
    """main.cpp:220-224 verbatim, integer arithmetic."""
    r = (n - 1) % m + 1
    return n + (m - r)


def predict_grid(bbox_min, bbox_max, resolution, padding):
    """Read-only mirror of main.cpp:418-442 (bbox -> pad by padding*max_extent
    -> per-axis scale of `resolution` -> nearest multiple of 8 -> cubic-voxel
    rescale of Y/Z world sizes). np.float32 throughout to mirror the C++
    float arithmetic, including the int() truncation before rounding to 8.
    Returns (dims[3] int, world_size[3] f32, center[3] f32)."""
    bbox_min = bbox_min.astype(np.float32)
    bbox_max = bbox_max.astype(np.float32)
    size = bbox_max - bbox_min
    wmax = np.float32(size.max())
    size = size + np.float32(padding) * wmax
    wmax = np.float32(size.max())
    dims = np.array([nearest_multiple_of(int(np.float32(resolution) * (s / wmax)), 8)
                     for s in size], dtype=np.int64)
    size_out = size.copy()
    size_out[1] = np.float32(dims[1]) * size[0] / np.float32(dims[0])
    size_out[2] = np.float32(dims[2]) * size[0] / np.float32(dims[0])
    center = np.float32(0.5) * (bbox_min + bbox_max)
    return dims, size_out, center


def verify_grid(data, resolution, padding):
    """Predictor + provenance checks. Exit 0 = prediction matches the VAC
    grid; exit 1 = mismatch (message names the remedy)."""
    bbox_min = data[:, :3].min(axis=0).astype(np.float64)
    bbox_max = data[:, :3].max(axis=0).astype(np.float64)
    ext = bbox_max - bbox_min
    mid = 0.5 * (bbox_min + bbox_max)

    # 1. bbox midpoint must equal the VAC grid center to sub-voxel — the fit
    #    centers on the midpoint and padding doesn't move it. A miss means
    #    this catalog is NOT the VAC input: stop and escalate (design §6.1).
    dmid = np.abs(mid - np.array(VAC_CENTER))
    print(f'bbox midpoint {mid}')
    print(f'VAC center    {VAC_CENTER}   |delta| {dmid} Mpc (voxel {VAC_VOXEL})')
    if (dmid >= VAC_VOXEL).any():
        sys.exit('FATAL: bbox midpoint does not match the VAC grid center to '
                 'sub-voxel — this catalog is not the VAC input. STOP and '
                 'escalate to the human (design §6.1 step 1).')

    # 2. Back-solve the padding: p = (published_size - extent) / max_extent.
    #    X is exact; Y/Z published sizes are post-cubic-rescale so those two
    #    are approximate (rounding-to-8 effects). All three ~= 0.1 is a free
    #    provenance check that the VAC ran at GRID_PADDING = 0.1.
    p = (np.array(VAC_SIZE) - ext) / ext.max()
    print(f'back-solved GRID_PADDING per axis: {p}  (config.polyp default 0.1; '
          f'X exact, Y/Z approximate post-rescale)')

    # 3. Predict the C++ fit at the requested knobs.
    dims, size, center = predict_grid(bbox_min, bbox_max, resolution, padding)
    print(f'predicted dims @ resolution {resolution}, padding {padding}: '
          f'{tuple(int(d) for d in dims)}   (VAC: {VAC_DIMS})')
    print(f'predicted world size: {size}   (VAC: {VAC_SIZE})')
    expected_dims = VAC_DIMS if resolution == 1200 else \
        tuple(d // (1200 // resolution) for d in VAC_DIMS) if 1200 % resolution == 0 else None
    if tuple(int(d) for d in dims) == VAC_DIMS and \
       np.allclose(size, VAC_SIZE, rtol=2e-3):
        print('PREDICTION MATCHES the published VAC grid.')
        return
    if resolution != 1200:
        # Fallback-resolution runs (e.g. 600 -> expect 356x600x364) are
        # checked by eye against the printed prediction; only the native
        # resolution is asserted against VAC_DIMS.
        print(f'(non-native resolution {resolution}: expected roughly '
              f'{expected_dims}; empirical check is the --headless run)')
        return
    sys.exit('MISMATCH at native resolution. First remedy: set config.polyp '
             f'"Grid padding" to the back-solved X value {p[0]:.6f} and re-run '
             '--verify-grid (documented config choice, no code change). If NO '
             'single padding value reproduces the grid, STOP and escalate to '
             'the human with the residual in voxels — forking the quirk-'
             'preserved C++ fit needs explicit sign-off (design §6.1).')
```

Then replace the existing `main()` (from Task 5) in the same file with:

```python
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--csv', default=CSV_DEFAULT)
    ap.add_argument('--out', default=OUT_DEFAULT,
                    help='output base path (no extension)')
    ap.add_argument('--verify-grid', action='store_true',
                    help='predict the C++ grid fit instead of packing (read-only)')
    ap.add_argument('--resolution', type=int, default=1200,
                    help='GRID_RESOLUTION knob to predict with (600 = fallback)')
    ap.add_argument('--padding', type=float, default=0.1,
                    help='GRID_PADDING knob to predict with')
    args = ap.parse_args()
    data = load_catalog(args.csv)
    if args.verify_grid:
        verify_grid(data, args.resolution, args.padding)
    else:
        pack(data, args.out)
    return 0
```

- [ ] **Step 2: Run the predictor**

Run: `cd /Users/rulkens/Development/vendor/cpp/Polyphorm && python3 tools/pack_vac_catalog.py --verify-grid`
Expected: midpoint deltas all < 0.78131 Mpc; back-solved padding ≈ 0.1 on all axes; `predicted dims @ resolution 1200, padding 0.1: (712, 1200, 728)` and `PREDICTION MATCHES the published VAC grid.`
If it exits with MISMATCH: follow the printed remedy — set `Grid padding` in Step 3 to the back-solved X value and re-run; if no single padding works, STOP the task and escalate to the human with the residual quantified in voxels.
If it exits with the midpoint FATAL: STOP the milestone and escalate — the catalog is not the VAC input.

- [ ] **Step 3: Set the run config**

In `bin/config.polyp`, change line 2:

```
Grid resolution = 1024
```

to:

```
Grid resolution = 1200
```

(Use the Task 1 verdict: if the fallback was chosen there, write `600` instead and note it in the run log.) Leave `Grid padding = 0.1` unless Step 2's remedy said otherwise.

- [ ] **Step 4: Add the "SDSS VAC" constants block**

In `main.cpp`, the active `REGIME_SDSS` constants (`// SDSS large`) do NOT match the VAC metadata (3.51/0.89/4.08 vs 4.6/0.8/2.5). Following the file's own commented-alternates idiom, replace:

```cpp
// SDSS large
const float SENSE_SPREAD = 20.0;
const float SENSE_DISTANCE = 3.51;
const float MOVE_ANGLE = 10.0;
const float MOVE_DISTANCE = 0.1;
const float AGENT_DEPOSIT = 0.0;
const float PERSISTENCE = 0.89;
const float SAMPLING_EXPONENT = 4.08;
```

with:

```cpp
// SDSS large
// const float SENSE_SPREAD = 20.0;
// const float SENSE_DISTANCE = 3.51;
// const float MOVE_ANGLE = 10.0;
// const float MOVE_DISTANCE = 0.1;
// const float AGENT_DEPOSIT = 0.0;
// const float PERSISTENCE = 0.89;
// const float SAMPLING_EXPONENT = 4.08;
// SDSS VAC — published DR17 Cosmic Slime VAC parameters (its
// export_metadata.txt: sense 4.6 mpc, persistence 0.8, sharpness 2.5;
// move 0.1 / spreads 20,10 / deposit 0 already matched). M5 validation
// protocol; a data edit in the file's own alternate-block idiom, not a
// logic change (design §7.1).
const float SENSE_SPREAD = 20.0;
const float SENSE_DISTANCE = 4.6;
const float MOVE_ANGLE = 10.0;
const float MOVE_DISTANCE = 0.1;
const float AGENT_DEPOSIT = 0.0;
const float PERSISTENCE = 0.8;
const float SAMPLING_EXPONENT = 2.5;
```

- [ ] **Step 5: Build + full suite**

Run: `cmake -B build && nice -n 19 cmake --build build -j 8 && cd build && ctest --output-on-failure`
Expected: all 7 suites green. Note: energy_smoke now runs the synthetic dataset under the VAC constants (it generates its own `config.polyp`, so the resolution change doesn't touch it, but the compile-time constants do). If energy_smoke fails, STOP and report to the human — do not tweak the test and do not silently revert the constants.

- [ ] **Step 6: Commit**

```bash
git add tools/pack_vac_catalog.py bin/config.polyp main.cpp
git commit -m "feat: VAC grid predictor + run config (resolution 1200, SDSS VAC constants)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Empirical grid anchor + full-scale allocation probe

**Files:**
- Modify: `docs/superpowers/research/m5/m5-run-log.md` (append probe section)

**Interfaces:**
- Consumes: packed dataset (Task 5), run config (Task 6), `--export` (Task 4).
- Produces: empirical confirmation that the C++ auto-fit yields 712×1200×728 at full scale and that allocation + a sim step + the export transient fit in memory (this completes Task 1's deferred design-§6.2 probe). Gates Task 9's long run.

- [ ] **Step 1: Run the probe (short — ~1–3 min; 600 s timeout)**

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm/bin && \
/usr/bin/time -l ../build/polyphorm --headless 60 --export \
  --dataset "data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0" \
  2>&1 | tee /private/tmp/claude-501/-Users-rulkens-Development-vendor-cpp-Polyphorm/f4b860ea-f9da-442a-84db-2bb65801a732/scratchpad/m5-probe.log
```

Expected in the output:
- `-> input data points: 324901`
- `-> simulation grid resolution: 712 x 1200 x 728`
- `-> simulation domain: 556.29 x 937.56 x 568.79 Mpc` (2-dp printf of the VAC sizes)
- `[headless] iteration ...` E lines, no `[gpu] uncaptured error`, no `FATAL`
- `Done exporting simulation data.` and `/usr/bin/time -l`'s `maximum resident set size`.

**If the grid line is wrong:** apply the back-solved `GRID_PADDING` from Task 6 Step 2 to `bin/config.polyp` and re-run; if still wrong, STOP and escalate to the human with the residual in voxels (design §6.1).
**If allocation fails** (Dawn names the failing limit fatally at startup): switch to the documented fallback — `Grid resolution = 600` in `bin/config.polyp`, verify with `python3 tools/pack_vac_catalog.py --verify-grid --resolution 600` (predicted 356×600×364), re-run the probe expecting that grid, and record that all subsequent comparison happens at d8 with factor 4. Commit the config change with the Task 6 files' message conventions.

- [ ] **Step 2: Verify the full-scale export contract**

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm/bin && python3 - <<'EOF'
import os, re
meta = open('export/export_metadata.txt').read()
assert 'dataset: data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0' in meta
x, y, z = map(int, re.search(r'grid resolution: (\d+) x (\d+) x (\d+)', meta).groups())
print('grid:', x, y, z)
for name in ('trace', 'deposit'):
    size = os.path.getsize(f'export/{name}.bin')
    assert size == x * y * z * 2, (name, size)
print('export sizes OK:', x * y * z * 2, 'bytes each')  # 1,243,929,600 at native
EOF
```

Expected: `grid: 712 1200 728` (or the documented fallback dims) and `export sizes OK`.

- [ ] **Step 3: Append the probe record to the run log**

Append to `docs/superpowers/research/m5/m5-run-log.md` (substitute the measured numbers):

```markdown
## Task 7: grid anchor + allocation probe (design §6.1 final check, §6.2 probe)

- Command: `--headless 60 --export --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0`
- Grid line: `712 x 1200 x 728` (matches the published VAC grid; predictor agreed)
- Domain line: `556.29 x 937.56 x 568.79 Mpc`
- GRID_PADDING used: <0.1 or the back-solved value, if changed>
- Peak RSS (`/usr/bin/time -l`): <measured> bytes
- Export sizes: trace.bin = deposit.bin = <measured> bytes (= X*Y*Z*2)
- Verdict: native-scale run + export CONFIRMED feasible (completes the Task
  zero probe). <or: fallback GRID_RESOLUTION=600 in effect, dims 356x600x364 —
  all reported numbers must state this resolution.>
```

- [ ] **Step 4: Full suite + commit**

Run: `cd build && ctest --output-on-failure` — all 7 suites green.

```bash
git add docs/superpowers/research/m5/m5-run-log.md
git commit -m "docs: m5 grid-anchor + allocation probe results

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: Comparison pipeline — `tools/validate/compare_trace.py`

**Files:**
- Create: `tools/validate/compare_trace.py`

**Interfaces:**
- Consumes: `<export-dir>/trace.bin` + `export_metadata.txt` (Task 3/4 on-disk format: headerless f16, Z-major/X-fastest); reference `mcpm_sdss_d8.npy` (read-only).
- Produces: CLI `compare_trace.py --export-dir D --reference R --out O [--orientation-scan] [--eps E]` plus `--self-test [--reference R]`; writes `report.json`, `report.txt`, `projections.png` into `--out`. Functions Task 10/11 rely on: `load_export(export_dir)`, `block_average_to_d8(cube)`, `metrics(ours_lin, ref_lin, eps)`, `orientation_scan(ours_d8, ref, eps)`, module constant `PINNED_FLIPS`. **Prints no pass/fail verdict.**

- [ ] **Step 1: Write the failing self-test invocation**

Run: `cd /Users/rulkens/Development/vendor/cpp/Polyphorm && python3 tools/validate/compare_trace.py --self-test`
Expected: FAIL — `No such file or directory` (script does not exist yet).

- [ ] **Step 2: Write the script**

Create `tools/validate/compare_trace.py`:

```python
#!/usr/bin/env python3
"""compare_trace.py — M5 validation comparison: the macOS port's exported
trace vs the published SDSS DR17 Cosmic Slime VAC, at the d8 tier.

Inputs:
  --export-dir : contains trace.bin (headerless raw f16, Z-major/X-fastest —
                 the axis order is implicit in the file, so it is pinned HERE)
                 and export_metadata.txt (grid dims).
  --reference  : mcpm_sdss_d8.npy — float32 (89, 150, 91) = (X, Y, Z),
                 block-averaged 8x from the published 712x1200x728 cube by
                 skymap's extractMcpmCube.py (downscale_local_mean).

Reports (NO pass/fail verdict — measure-first policy, M5 design §1: the
human sets the acceptance bar after the first honest measurement):
  - 3D voxelwise Pearson on log10(x+eps): masked (joint support) + unmasked
  - per-axis max-projection Pearson (project linear first, then log):
    masked + unmasked  -> eight numbers total
  - eps sensitivity (1e-4 / 1e-2) as one-line summaries
  - projections.png: ours vs reference max-projections, magma, shared scale

Conventions mirror rhizome's compareCubes.py (per-axis max-projection
Pearson as first-class metrics; magma log-scaled render; np.corrcoef equals
its manual Pearson formula). Deliberate, documented deviations (design §8):
log10(x+eps) instead of linear min-max; block-average instead of trilinear
zoom (same operator that produced the reference tiers); masked + unmasked
variants (~70% of d8 reference voxels are exact zeros).

Offline only: numpy, scikit-image, matplotlib. No network.
"""
import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from skimage.transform import downscale_local_mean

D8_SHAPE = (89, 150, 91)   # (X, Y, Z) — extractMcpmCube.py convention
# eps rationale (design §8.3): d8 nonzero minimum ~= 3.9e-4, so 1e-3 sits at
# the bottom of the signal range — no -inf, negligible compression of real
# structure, round number. New explicit choice (compareCubes.py is linear
# min-max; no local log-eps precedent). Sensitivity reported at 1e-4 / 1e-2.
DEFAULT_EPS = 1e-3
SENSITIVITY_EPS = (1e-4, 1e-2)

# Orientation pin (design §8.6): at d8 the dims (89, 150, 91) are pairwise
# distinct, so the axis TRANSPOSITION is pinned by shape alone; only the 8
# reflection combinations need the one-time --orientation-scan on the first
# real export. (False, False, False) = identity. If a flip ever wins the
# scan, that is a BUG in load_export's layout assumption — fix the transpose
# there; never ship a compensating flip here. The recorded scan table lives
# in docs/superpowers/research/m5/m5-run-log.md.
PINNED_FLIPS = (False, False, False)


def parse_dims(metadata_path):
    """(X, Y, Z) ints from export_metadata.txt's grid-resolution line."""
    text = Path(metadata_path).read_text()
    m = re.search(r'simulation grid resolution: (\d+) x (\d+) x (\d+)', text)
    if not m:
        sys.exit(f'FATAL: no grid resolution line in {metadata_path}')
    return tuple(int(g) for g in m.groups())


def load_export(export_dir):
    """trace.bin -> float32 cube indexed (X, Y, Z).

    The .bin is headerless raw f16 with index = z*W*H + y*W + x (Z-major/
    X-fastest — DirectXTex layout; export-path research §1/§6). Reshape
    (Z, Y, X) C-order, then transpose to (X, Y, Z) to match the reference's
    convention. PINNED_FLIPS is applied last (identity unless the one-time
    scan ever proves otherwise — see the constant's comment).
    """
    export_dir = Path(export_dir)
    x, y, z = parse_dims(export_dir / 'export_metadata.txt')
    raw = np.fromfile(export_dir / 'trace.bin', dtype=np.float16)
    if raw.size != x * y * z:
        sys.exit(f'FATAL: trace.bin has {raw.size} voxels, metadata says {x*y*z}')
    cube = raw.reshape(z, y, x).astype(np.float32).transpose(2, 1, 0)
    for axis, flip in enumerate(PINNED_FLIPS):
        if flip:
            cube = np.flip(cube, axis)
    return np.ascontiguousarray(cube)


def block_average_to_d8(cube):
    """Mean-preserving block average to D8_SHAPE using the SAME operator
    (skimage downscale_local_mean) that produced the reference tiers.
    Hard-fails unless the factors are exact integers (8 at native, 4 at the
    half-res fallback). float32 accumulation: f16 quantization (~1e-3
    relative) already dominates any f32 accumulation error over <=512-voxel
    blocks, so float64 buys nothing at 8x the memory (design §8.2)."""
    factors = []
    for s, t in zip(cube.shape, D8_SHAPE):
        if s % t != 0:
            sys.exit(f'FATAL: non-integer downsample factor {cube.shape} -> {D8_SHAPE}')
        factors.append(s // t)
    return downscale_local_mean(cube, tuple(factors)).astype(np.float32)


def pearson(a, b):
    """Pearson on flattened arrays; np.corrcoef == compareCubes.py's manual
    dot-product formula."""
    return float(np.corrcoef(np.ravel(a).astype(np.float64),
                             np.ravel(b).astype(np.float64))[0, 1])


def metrics(ours_lin, ref_lin, eps):
    """The eight numbers (design §8.4/§8.5), from LINEAR d8 cubes.

    Masking: primary = joint support (nonzero in BOTH cubes pre-log,
    "exclude-zeros-in-either") — including the ~70% co-located zeros adds a
    constant-value mass at log10(eps) that inflates Pearson by rewarding
    agreement on empty background. Unmasked variants are also reported (the
    PolyPhy precedent numbers are unmasked; comparability matters).
    Projections: max-project the LINEAR cube first, then log10(+eps), then
    Pearson per axis (compareCubes.py's structure-over-intensity rationale).
    """
    out = {}
    lo = np.log10(ours_lin + eps)
    lr = np.log10(ref_lin + eps)
    joint = (ours_lin > 0) & (ref_lin > 0)
    out['pearson3d_masked'] = pearson(lo[joint], lr[joint])
    out['pearson3d_unmasked'] = pearson(lo, lr)
    out['joint_support_voxels'] = int(joint.sum())
    out['total_voxels'] = int(joint.size)
    for ax, name in enumerate('xyz'):
        po = ours_lin.max(axis=ax)
        pr = ref_lin.max(axis=ax)
        plo = np.log10(po + eps)
        plr = np.log10(pr + eps)
        pj = (po > 0) & (pr > 0)
        out[f'pearson_proj_{name}_masked'] = pearson(plo[pj], plr[pj])
        out[f'pearson_proj_{name}_unmasked'] = pearson(plo, plr)
    return out


def orientation_scan(ours_d8, ref, eps):
    """Unmasked axis-projection Pearsons under all 8 flip combinations
    (design §8.6). Identity must win decisively on the first real export;
    then the finding is pinned in PINNED_FLIPS and routine runs skip this.
    A winning flip = layout bug in load_export — fix the transpose there."""
    print('orientation scan (8 flip combos, sorted by mean; identity must win):')
    print('  flips (x, y, z)             proj-x    proj-y    proj-z     mean')
    results = []
    for fx in (False, True):
        for fy in (False, True):
            for fz in (False, True):
                cube = ours_d8
                for axis, f in enumerate((fx, fy, fz)):
                    if f:
                        cube = np.flip(cube, axis)
                rs = [pearson(np.log10(cube.max(axis=ax) + eps),
                              np.log10(ref.max(axis=ax) + eps))
                      for ax in range(3)]
                results.append(((fx, fy, fz), rs, float(np.mean(rs))))
    for flips, rs, mean in sorted(results, key=lambda t: -t[2]):
        tag = '   <-- identity' if flips == (False, False, False) else ''
        print(f'  {str(flips):26s} {rs[0]:+.4f}   {rs[1]:+.4f}   {rs[2]:+.4f}   '
              f'{mean:+.4f}{tag}')
    return results


def render_projections(ours_d8, ref, eps, out_png):
    """Side-by-side max-projection PNG per axis: ours vs reference,
    log10(+eps), magma, shared color scale per axis row (compareCubes.py
    precedent: imshow(img.T, origin='lower', cmap='magma'))."""
    fig, axes = plt.subplots(3, 2, figsize=(9, 13))
    for ax_i, name in enumerate('xyz'):
        po = np.log10(ours_d8.max(axis=ax_i) + eps)
        pr = np.log10(ref.max(axis=ax_i) + eps)
        vmin = float(min(po.min(), pr.min()))
        vmax = float(max(po.max(), pr.max()))
        for col, (img, label) in enumerate(((po, 'ours'), (pr, 'reference'))):
            a = axes[ax_i][col]
            a.imshow(img.T, origin='lower', cmap='magma', vmin=vmin, vmax=vmax)
            a.set_title(f'{label} — {name} max-projection (log10+{eps:g})')
            a.axis('off')
    fig.tight_layout()
    fig.savefig(out_png, dpi=120, bbox_inches='tight')
    plt.close(fig)


def run_compare(export_dir, reference, out_dir, eps, do_scan):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    ref = np.load(reference).astype(np.float32)
    if ref.shape != D8_SHAPE:
        sys.exit(f'FATAL: reference shape {ref.shape} != {D8_SHAPE}')
    ours_native = load_export(export_dir)
    native_dims = tuple(int(s) for s in ours_native.shape)
    ours_d8 = block_average_to_d8(ours_native)
    del ours_native   # free ~2.5 GB before metrics/render

    if do_scan:
        orientation_scan(ours_d8, ref, eps)

    report = {
        'export_dir': str(export_dir),
        'reference': str(reference),
        'native_dims_xyz': list(native_dims),
        'downsample_factors': [s // t for s, t in zip(native_dims, D8_SHAPE)],
        'pinned_flips': list(PINNED_FLIPS),
        'eps_primary': eps,
        'metrics_by_eps': {},
    }
    for e in sorted(set((eps,) + SENSITIVITY_EPS)):
        report['metrics_by_eps'][f'{e:g}'] = metrics(ours_d8, ref, e)

    render_projections(ours_d8, ref, eps, out_dir / 'projections.png')

    m = report['metrics_by_eps'][f'{eps:g}']
    lines = [
        f'M5 d8 comparison — ours (native {native_dims}, block-averaged '
        f'x{report["downsample_factors"]}) vs {Path(reference).name}',
        f'primary eps = {eps:g}   joint support = {m["joint_support_voxels"]}'
        f'/{m["total_voxels"]} voxels',
        '',
        f'{"metric":34s} {"masked":>10s} {"unmasked":>10s}',
        f'{"pearson 3D voxelwise":34s} {m["pearson3d_masked"]:>+10.4f} '
        f'{m["pearson3d_unmasked"]:>+10.4f}',
    ]
    for name in 'xyz':
        lines.append(f'{"pearson " + name + " max-projection":34s} '
                     f'{m["pearson_proj_" + name + "_masked"]:>+10.4f} '
                     f'{m["pearson_proj_" + name + "_unmasked"]:>+10.4f}')
    lines.append('')
    for e in SENSITIVITY_EPS:
        s = report['metrics_by_eps'][f'{e:g}']
        lines.append(f'eps {e:g}: 3D {s["pearson3d_masked"]:+.4f} masked / '
                     f'{s["pearson3d_unmasked"]:+.4f} unmasked (sensitivity)')
    lines.append('')
    lines.append('NOTE: no pass/fail verdict is printed — measure-first policy '
                 '(M5 design §1); the human sets the acceptance bar.')
    text = '\n'.join(lines) + '\n'
    (out_dir / 'report.txt').write_text(text)
    (out_dir / 'report.json').write_text(json.dumps(report, indent=2))
    print(text)
    print(f'wrote {out_dir}/report.json, report.txt, projections.png')


def self_test(reference=None):
    """Synthetic-cube verification of every pipeline stage. Seeded RNG:
    deterministic. Passes silently assert; prints a summary on success."""
    rng = np.random.default_rng(20260813)
    # (a) pearson: exact known — b = (a+n)/sqrt(2) has corr 1/sqrt(2).
    a = rng.standard_normal(D8_SHAPE)
    n = rng.standard_normal(D8_SHAPE)
    b = (a + n) / np.sqrt(2.0)
    r = pearson(a, b)
    assert abs(r - 1.0 / np.sqrt(2.0)) < 0.005, r
    # (b) metrics pipeline recovers the same correlation through log10(+eps)
    #     (10**a is always > 0, so the joint mask covers everything and the
    #     eps floor only nibbles the far-negative tail).
    m = metrics(np.power(10.0, a).astype(np.float32),
                np.power(10.0, b).astype(np.float32), DEFAULT_EPS)
    assert abs(m['pearson3d_masked'] - 1.0 / np.sqrt(2.0)) < 0.02, m
    assert m['joint_support_voxels'] == m['total_voxels']
    # (c) block averaging: a repeat-upsampled cube must come back exactly
    #     (also exercises the half-res factor path, 2 here vs 4 in prod).
    d8 = rng.random(D8_SHAPE).astype(np.float32)
    up = d8.repeat(2, axis=0).repeat(2, axis=1).repeat(2, axis=2)
    assert np.allclose(block_average_to_d8(up), d8, atol=1e-6)
    # (d) load_export layout pin: synthetic Z-major/X-fastest f16 export.
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        X, Y, Z = 3, 4, 5
        xs, ys, zs = np.meshgrid(np.arange(X), np.arange(Y), np.arange(Z),
                                 indexing='ij')
        pattern = (xs + 10 * ys + 100 * zs).astype(np.float32)  # exact in f16
        pattern.transpose(2, 1, 0).astype(np.float16).tofile(td / 'trace.bin')
        (td / 'export_metadata.txt').write_text(
            f'simulation grid resolution: {X} x {Y} x {Z} [vox]\n')
        cube = load_export(td)
        assert cube.shape == (X, Y, Z), cube.shape
        assert cube[1, 2, 3] == 1 + 20 + 300
        assert cube[2, 0, 4] == 2 + 0 + 400
    # (e) against the real reference, when provided (design §11 item 7 smoke):
    #     identity -> 1.0; shuffled -> ~0. Exercises masking on the real
    #     ~70%-zero field.
    if reference is not None:
        ref = np.load(reference).astype(np.float32)
        assert ref.shape == D8_SHAPE, ref.shape
        m_self = metrics(ref, ref, DEFAULT_EPS)
        assert m_self['pearson3d_masked'] > 0.999999, m_self
        assert m_self['pearson3d_unmasked'] > 0.999999, m_self
        shuffled = rng.permutation(ref.ravel()).reshape(ref.shape)
        m_shuf = metrics(ref, shuffled, DEFAULT_EPS)
        assert abs(m_shuf['pearson3d_unmasked']) < 0.01, m_shuf
        print('self-test vs reference: identity == 1.0, shuffled ~ 0  OK')
    print('compare_trace.py self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--export-dir')
    ap.add_argument('--reference')
    ap.add_argument('--out')
    ap.add_argument('--eps', type=float, default=DEFAULT_EPS)
    ap.add_argument('--orientation-scan', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test(args.reference)
        return 0
    if not (args.export_dir and args.reference and args.out):
        ap.error('--export-dir, --reference and --out are required unless --self-test')
    run_compare(args.export_dir, args.reference, args.out, args.eps,
                args.orientation_scan)
    return 0


if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 3: Run the synthetic self-test**

Run: `cd /Users/rulkens/Development/vendor/cpp/Polyphorm && python3 tools/validate/compare_trace.py --self-test`
Expected: `compare_trace.py self-test passed`.

- [ ] **Step 4: Run the self-test against the real reference cube**

Run: `python3 tools/validate/compare_trace.py --self-test --reference ~/Development/js/skymap/data/raw/mcpm/mcpm_sdss_d8.npy`
Expected: `self-test vs reference: identity == 1.0, shuffled ~ 0  OK` then `compare_trace.py self-test passed`.

- [ ] **Step 5: Full suite + commit**

Run: `cd build && ctest --output-on-failure` — all 7 suites green (no C++ changed; constraint applies before every commit regardless).

```bash
git add tools/validate/compare_trace.py
git commit -m "feat: d8 comparison pipeline (tools/validate/compare_trace.py)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Launch the validation run (background)

**Files:**
- Modify: `docs/superpowers/research/m5/m5-run-log.md` (append launch record)
- Creates (by the run, later): `docs/superpowers/research/m5/run1-headless.log`, `bin/export/{trace,deposit}.bin`, `export_metadata.txt`, `halos_measurements.csv`

**Interfaces:**
- Consumes: everything from Tasks 4–7.
- Produces: a running background sim whose completion Task 10 consumes. **This task's deliverable ends at "run launched + early iterations sane" (Global Constraints) — do NOT wait for completion here, and never fabricate or predict its results.**

- [ ] **Step 1: Launch (run_in_background, output to a log file)**

Launch via the Bash tool with `run_in_background: true`:

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm/bin && \
/usr/bin/time -l ../build/polyphorm --headless 1000 --export \
  --dataset "data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0" \
  > ../docs/superpowers/research/m5/run1-headless.log 2>&1
```

(Protocol per design §7.3: spec guidance "~1000+ iterations, energy plateau"; `E` prints every 50 iterations; the export fires on the final frame via `--export`.)

- [ ] **Step 2: Early-sanity check (~2 minutes after launch)**

Run: `grep -E 'grid resolution|headless|FATAL|uncaptured' /Users/rulkens/Development/vendor/cpp/Polyphorm/docs/superpowers/research/m5/run1-headless.log`
Expected: `-> simulation grid resolution: 712 x 1200 x 728` (or the documented fallback), at least one `[headless] iteration ... E = ...` line with E > 0, and NO `FATAL` / `uncaptured error` lines. If sanity fails, kill the run, diagnose, and do not proceed.

- [ ] **Step 3: Append the launch record to the run log**

Append to `docs/superpowers/research/m5/m5-run-log.md` (substitute measured values):

```markdown
## Task 9: validation run 1 launched (protocol §7)

- Command: `--headless 1000 --export --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0`
- Log: run1-headless.log (same dir); launched <date/time>, git rev <rev>
- config.polyp: Agents 10000000, Grid resolution <1200/600>, Grid padding <value>
- Compile-time constants: SDSS VAC (sense 4.6, persist 0.8, sharp 2.5,
  move 0.1, spreads 20/10, deposit 0)
- Known unrecorded-by-VAC parameters (design §7.2, interpretation caveats):
  iteration count at export (protocol choice: 1000 + plateau criterion),
  RNG seeding (upstream unseeded), GRID_PADDING (back-solved), HISTOGRAM_BASE
  (stats-only).
- Early sanity at ~2 min: grid line correct, E series printing, no GPU errors.
```

- [ ] **Step 4: Full suite + commit (the run keeps going in the background)**

Run: `cd build && ctest --output-on-failure` — all 7 suites green. (energy_smoke launches its own short headless process; it coexists with the background run — Metal time-slices. If a suite fails with a device-allocation error while the big run holds memory, note it and re-run the suite after Task 10's completion wait instead — do not kill the validation run to make a test pass, and do not commit until the suites are green.)

```bash
git add docs/superpowers/research/m5/m5-run-log.md
git commit -m "docs: m5 validation run launched

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: Convergence check + metadata parity + orientation scan pin

**Files:**
- Modify: `docs/superpowers/research/m5/m5-run-log.md` (convergence + scan table)
- Create (commit): `docs/superpowers/research/m5/run1-headless.log` (the E series record — design §7.3 "record the E series alongside the export")
- Possibly modify: `tools/validate/compare_trace.py` (only the `PINNED_FLIPS` comment, or a transpose FIX if the scan exposes a layout bug)

**Interfaces:**
- Consumes: the completed Task 9 run (wait for the background-completion notification — never predict it); `compare_trace.py --orientation-scan` (Task 8); reference `export_metadata.txt`.
- Produces: a validated, converged export + the pinned orientation finding. Gates Task 11.

- [ ] **Step 1: Confirm completion and convergence**

After the background run completes, check the tail: `tail -20 docs/superpowers/research/m5/run1-headless.log` — expect `Done exporting simulation data.`, `ENERGY RISING`, and `/usr/bin/time -l` stats. Then evaluate the plateau criterion (design §7.3: relative ΔE over the final 200 iterations < 1%):

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm && python3 - <<'EOF'
import re
text = open('docs/superpowers/research/m5/run1-headless.log').read()
es = {int(m[1]): float(m[2]) for m in
      re.finditer(r'\[headless\] iteration (\d+)\s+E = ([0-9.eE+-]+)', text)}
its = sorted(es)
last = its[-1]
prev = last - 200
assert prev in es, (last, its[:5])
delta = abs(es[last] - es[prev]) / es[prev]
print(f'E({prev}) = {es[prev]}   E({last}) = {es[last]}   relative dE = {delta:.4%}')
print('CONVERGED (dE < 1% over final 200 iterations)' if delta < 0.01
      else 'NOT CONVERGED — rerun longer (2000) per protocol §7.3')
EOF
```

If NOT CONVERGED: relaunch Task 9's command with `--headless 2000` (fresh run — there is no checkpointing) into `run2-headless.log`, append a run-log entry, wait, and repeat this step against the new log. Do not proceed unconverged.

- [ ] **Step 2: Field-by-field metadata diff vs the reference (design §7.1 post-run check)**

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm/bin && python3 - <<'EOF'
import os, re, sys
ours = open('export/export_metadata.txt').read()
ref = open(os.path.expanduser(
    '~/Development/js/skymap/data/raw/mcpm/export_metadata.txt')).read()
def kv(t):
    return {l.split(':', 1)[0].strip(): l.split(':', 1)[1].strip()
            for l in t.strip().splitlines() if ':' in l}
o, r = kv(ours), kv(ref)
exact = ['dataset', 'number of agents', 'simulation grid resolution']
fail = False
for k in exact:
    ok = o.get(k) == r.get(k)
    print(f'{"PASS" if ok else "FAIL"}  {k}: ours={o.get(k)!r} ref={r.get(k)!r}')
    fail |= not ok
# Known surrogate delta (design risk 4): 324901 vs 324849 — INFO, not failure.
print(f'INFO  number of data points: ours={o["number of data points"]} '
      f'ref={r["number of data points"]} (52-pt surrogate delta, documented)')
numeric = ['simulation grid size', 'simulation grid center', 'move distance',
           'move distance grid', 'sense distance', 'sense distance grid',
           'move spread', 'sense spread', 'persistence coefficient',
           'agent deposit', 'sampling sharpness']
for k in numeric:
    on = [float(v) for v in re.findall(r'-?\d+\.?\d*(?:[eE][+-]?\d+)?', o[k])]
    rn = [float(v) for v in re.findall(r'-?\d+\.?\d*(?:[eE][+-]?\d+)?', r[k])]
    ok = len(on) == len(rn) and all(
        abs(a - b) <= 2e-3 * max(abs(a), abs(b), 1e-6) for a, b in zip(on, rn))
    print(f'{"PASS" if ok else "FAIL"}  {k}: ours={on} ref={rn}')
    fail |= not ok
sys.exit(1 if fail else 0)
EOF
```

Expected: all PASS + the one INFO line. **Any FAIL invalidates the run before comparison** (design §7.1): fix the config/constants mismatch, relaunch (Task 9 procedure), and repeat. (At the half-res fallback, `simulation grid resolution` and the `grid`-unit lines legitimately differ — record that the diff was run in fallback mode and check only the Mpc-unit and dimensionless fields.)

- [ ] **Step 3: One-time orientation scan (before any reported number — design risk 6)**

Run:

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm && \
python3 tools/validate/compare_trace.py \
  --export-dir bin/export \
  --reference ~/Development/js/skymap/data/raw/mcpm/mcpm_sdss_d8.npy \
  --out docs/superpowers/research/m5/first-measurement \
  --orientation-scan
```

Expected: the 8-row scan table with `(False, False, False)   <-- identity` ranked first by mean, decisively (record the margin to the runner-up). Copy the full table into a new run-log section (Step 4).
**If a flip outranks identity:** that is a bug in `load_export`'s layout assumption — fix the reshape/transpose in `load_export`, re-run `--self-test` (it must still pass, including check (d)), re-run the scan, and only then continue. **Never** encode a compensating flip in `PINNED_FLIPS`.

- [ ] **Step 4: Record + pin**

Append to `docs/superpowers/research/m5/m5-run-log.md`:

```markdown
## Task 10: convergence, metadata parity, orientation (run 1)

- Convergence: E(<prev>) = <val>, E(<last>) = <val>, relative dE = <val> (< 1%: CONVERGED)
- Metadata diff vs reference export_metadata.txt: all fields PASS
  (data-point count 324901 vs 324849 — known surrogate delta, design risk 4)
- Orientation scan (design §8.6) — recorded table:
  <paste the printed 8-row table verbatim>
- Finding: identity wins by <margin> mean-Pearson over the best flip;
  PINNED_FLIPS stays (False, False, False).
```

In `tools/validate/compare_trace.py`, extend the `PINNED_FLIPS` comment's last line to: `# Scan run on run 1 (2026-08-<dd>): identity won — table recorded in docs/superpowers/research/m5/m5-run-log.md.`

- [ ] **Step 5: Full suite + commit**

Run: `cd build && ctest --output-on-failure` — all 7 suites green.

```bash
git add docs/superpowers/research/m5/m5-run-log.md \
        docs/superpowers/research/m5/run1-headless.log \
        tools/validate/compare_trace.py
git commit -m "docs: m5 convergence + metadata parity + orientation pin

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

(If Step 1 required a rerun, also add `run2-headless.log` and reference it as the run of record.)

---

### Task 11: First measurement report + HUMAN GATE

**Files:**
- Create (commit): `docs/superpowers/research/m5/first-measurement/{report.json,report.txt,projections.png}`
- Modify: `docs/superpowers/research/m5/m5-run-log.md` (first-measurement section)

**Interfaces:**
- Consumes: the validated export (Task 10), `compare_trace.py` (Task 8).
- Produces: M5's deliverable — the first honest numbers + sanity PNG, presented to the human, who sets the acceptance bar. **This plan encodes no threshold and renders no verdict.**

- [ ] **Step 1: Run the full comparison**

```bash
cd /Users/rulkens/Development/vendor/cpp/Polyphorm && \
python3 tools/validate/compare_trace.py \
  --export-dir bin/export \
  --reference ~/Development/js/skymap/data/raw/mcpm/mcpm_sdss_d8.npy \
  --out docs/superpowers/research/m5/first-measurement
```

Expected: the eight-number table (3D masked/unmasked + three axis projections masked/unmasked) at eps 1e-3, the eps 1e-4/1e-2 sensitivity lines, the no-verdict note, and `report.json` / `report.txt` / `projections.png` written. Eyeball `projections.png` for gross misalignment/mirroring (design §8.7) before trusting any number.

- [ ] **Step 2: Write the first-measurement record**

Append to `docs/superpowers/research/m5/m5-run-log.md` (substitute the measured numbers verbatim from `report.txt`):

```markdown
## Task 11: first measurement (M5 deliverable — design §1)

Resolution of record: <native 712x1200x728 | fallback 356x600x364> (the
report MUST state this — design §6.2). Iterations: <1000 or 2000>, plateau
dE = <value>. Provenance: git rev <rev>, SDSS VAC constants, config.polyp
resolution <value> / padding <value>, catalog = packed sample_3D_linW.csv
(324,901 pts).

### d8 log-trace Pearson (eps 1e-3)

| metric | masked (joint support) | unmasked |
|---|---|---|
| 3D voxelwise | <val> | <val> |
| x max-projection | <val> | <val> |
| y max-projection | <val> | <val> |
| z max-projection | <val> | <val> |

eps sensitivity: 1e-4 -> <3D masked/unmasked>; 1e-2 -> <3D masked/unmasked>.
Sanity PNG: first-measurement/projections.png.

### Interpretation caveats (design §12 — carried honestly)

1. PolyPhy precedent: ~0.085 3D / ~0.37-0.41 axis Pearson (linear min-max,
   unmasked) vs this same reference with the correct input after 11
   calibration runs — context, not a target.
2. Racy-deposit nondeterminism bounds attainable correlation; our runs are
   statistically, never bitwise, reproducible. (Optional post-gate follow-up
   the human may request: a second run to measure self-correlation as a
   ceiling estimate.)
3. The VAC's iteration count at export is unrecorded — our 1000-iteration
   plateau is a protocol choice.
4. Catalog surrogate gap: 324,901 vs 324,849 pts; 6% of positions differ by
   median ~0.6 Mpc (below the 0.78 Mpc voxel).

### Human gate

Presented to the human partner on <date>: the table above +
projections.png. Per the measure-first decision, the human sets the
acceptance bar now; misses against that bar trigger the spec's
quirk-by-quirk A/B hunts (post-M5). Human's decision: <recorded verbatim
after the gate>
```

- [ ] **Step 3: Full suite + commit the measurement**

Run: `cd build && ctest --output-on-failure` — all 7 suites green.

```bash
git add docs/superpowers/research/m5/first-measurement \
        docs/superpowers/research/m5/m5-run-log.md
git commit -m "docs: m5 first measurement report

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 4: HUMAN GATE — present and stop**

Present to the human partner: the eight-number table, the eps sensitivity lines, `projections.png`, and the caveats section. **The human sets the acceptance bar** (measure-first decision, design §1). Do not proceed to quirk A/B hunts, d2/d4 tiers, threshold judgments, or any "fixes" motivated by the numbers — all post-M5, human-directed. M5 is complete when the numbers and PNG have been presented and the human's decision is recorded in the run log (amend the `Human's decision:` line in a follow-up docs commit once given).
