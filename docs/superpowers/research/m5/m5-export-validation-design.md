# M5 design — export + VAC validation

Milestone M5 of the macOS WebGPU port (branch `macos-webgpu-port`).
Design inputs (cited throughout instead of re-derived):

- [export-path-research.md](export-path-research.md) — F6 code path, the
  `save_texture3D` stub, readback machinery, byte-format facts, grid fit.
- [validation-target-research.md](validation-target-research.md) — reference
  cubes, provenance, catalog lineage, methodology sketch, risk precedent.
- [Top-level port spec](../../specs/2026-08-10-macos-webgpu-port-design.md) —
  M5 section (lines 130-148), quirk-preservation rules, F6 contract.
- [m4b-carryovers.md](../m4/m4b-carryovers.md) — inherited open items
  (triage in §9).

Two user decisions (final, 2026-08-12) are baked in and not revisited here:
**(a) full-catalog validation** — convert PolyPhy's bundled
`sample_3D_linW.csv` (~324,901 pts ≈ the real VAC input) to Polyphorm's
`.bin`/`_metadata.txt`; the shipped 37k slice stays untouched as the
visual/demo dataset. **(b) measure-first success bar** — M5 delivers the
pipeline and the first honest numbers, not a pass/fail gate.

## 1. Goals and success criteria

Goals:

1. `graphics::save_texture3D` implemented — F6 produces byte-format-exact
   `export/deposit.bin` + `export/trace.bin` (headerless raw, f16,
   Z-major/X-fastest), completing the F6 on-disk contract
   (`export_metadata.txt` and `halos_measurements.csv` already work —
   export-path-research §1, §5).
2. A VAC-scale input catalog packed from
   `~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv`,
   with the sim grid anchored to the published VAC geometry
   (712×1200×728 vox, 556.288×937.564×568.789 Mpc, center
   (-239.469, -16.5618, 201.275) Mpc, 0.78131 Mpc isotropic voxels).
3. A repeatable offline comparison pipeline (Python, `tools/validate/`)
   reporting log-trace Pearson at the d8 tier against skymap's
   `mcpm_sdss_d8.npy`, both 3D voxelwise and per-axis max-projection.

**Success criteria (measure-first — explicit, per user decision).** M5 is
done when the pipeline runs end-to-end and reports the d8 log-trace
Pearson numbers (3D voxelwise AND all three axis max-projections, both
masked and unmasked variants) plus the sanity PNG, from a
converged-per-protocol run on the full catalog at the anchored grid.
**There is no numeric pass/fail threshold in M5.** The top-level spec's
"≥0.9 at d8" figure is superseded for this milestone: the acceptance bar
is set by the human *after* the first honest measurement, informed by the
PolyPhy precedent (~0.4 axis Pearson with the correct input —
validation-target-research §5) and the nondeterminism floor (§8). Misses
against whatever bar is then set trigger the spec's quirk-by-quirk A/B
hunts; that is post-M5 work.

## 2. Architecture

Five pieces, in dependency order:

| # | Component | Where | New/changed |
|---|---|---|---|
| 0 | Memory feasibility check (task zero) | manual + `--headless` run | none (procedure) |
| 1 | `save_texture3D` (3D readback + f32→f16 + Z-major pack) | `cpplib/graphics.cpp` | replace stub |
| 2 | Headless export trigger (`--export` flag) | `main.cpp` | ~4 lines |
| 3 | Catalog converter | `tools/pack_vac_catalog.py` | new |
| 4 | Grid-anchoring verification | inside converter (`--verify-grid`) | new |
| 5 | Comparison pipeline | `tools/validate/compare_trace.py` | new |

Plus a run protocol (§7) tying 0-5 together. `tools/` does not exist yet;
this milestone creates it (spec line 136 already names `tools/validate/`).

## 3. Component: `save_texture3D` exporter

File: `/Users/rulkens/Development/vendor/cpp/Polyphorm/cpplib/graphics.cpp`
(stub at 484-486; declaration `cpplib/graphics.h:165`). Signature is fixed
by the two `main.cpp` call sites (1182/1184/1185) and upstream parity:

```cpp
void save_texture3D(Texture3D *texture, std::string filename);
```

`filename` arrives extension-less (`"export/deposit"`, `"export/trace"`);
we append `.bin`. The upstream `.dds` sibling is dropped (spec line 127).

Recipe (extends the proven patterns cited in export-path-research §3):

1. **Guard.** Assert `texture->format == Format::R32_FLOAT` (the only
   format the port's exportable textures use, `main.cpp:538-539,546`);
   any other format hits `log`+early-return, not silent garbage. Dims come
   from `Texture3D::width/height/depth` (`cpplib/graphics.h:68-74`).
2. **GPU→CPU copy.** `padded_bytes_per_row = (width*4 + 255) & ~255`
   (Dawn's 256 B `bytesPerRow` rule — same as
   `tests/render_path_tests.cpp:123-164`); readback buffer
   (`MapRead|CopyDst`) of `padded_bytes_per_row * height * depth`; one
   `CopyTextureToBuffer` with `bytesPerRow = padded`, `rowsPerImage =
   height`, `Extent3D{width, height, depth}`; flush encoder; `MapAsync` +
   blocking `ProcessEvents` pump — the exact idiom of
   `capture_structured_buffer` (`graphics.cpp:877-907`). Synchronous
   same-frame stall is the established, deliberate convention
   (`graphics.cpp:887-889`). The readback buffer is transient (created
   and destroyed per call — F6 is a rare user action; no caching).
3. **CPU repack + convert.** Walk slices z ∈ [0,depth), rows y ∈
   [0,height): de-pad each row and convert each f32 texel to f16, writing
   tightly packed output at `out[z*width*height + y*width + x]` — exactly
   DirectXTex's tight Z-major/X-fastest layout (export-path-research §1).
4. **f32→f16.** New helper in `graphics.cpp`:
   `static uint16_t f32_to_f16(float v)`. On Apple clang use the
   `_Float16` cast (hardware IEEE 754 binary16 conversion,
   round-to-nearest-even — identical semantics to the GPU-side conversion
   upstream's R16F texture performed on store, so quantization matches
   upstream's pipeline stage-for-stage in rounding behavior even though
   ours happens at export instead of at every store). Handle nothing
   specially: `_Float16` casts define inf/NaN/subnormal behavior. Expose
   it (e.g. via a small header or `extern` declaration) so the spec's
   planned CTest unit ("f32→f16 export conversion", spec line 165) can
   drive it with known vectors incl. 0, denormals, 65504 (f16 max),
   overflow→inf, NaN.
5. **Write.** Single `std::ofstream(filename + ".bin", binary)` write of
   the packed buffer (`width*height*depth*2` bytes). No header, no
   endianness handling (both platforms little-endian, upstream never
   byte-swapped — export-path-research §6).

Byte-format parity consequences: `OpenPolyphorm.ipynb`, pyslime, and
skymap's `extractMcpmCube.py` read the output unmodified, and the file
remains directly ingestible by a future OpenVDB converter (a design
consideration only — the converter itself is post-M5, §10).

Peak transient CPU cost at native grid: padded readback ≈ 2.5 GB mapped +
1.2 GB f16 output per texture, sequential per call — covered by the §6
feasibility check.

## 4. Component: headless export trigger + metadata dataset line

F6 is a keypress (`main.cpp:1017`); headless mode (`--headless N`,
`main.cpp:349-360`) has no input. The validation run must be headless
(no GUI launches during M5 execution). Add:

- `--export` CLI flag (parsed alongside `--headless`/`--dataset`,
  `main.cpp:355-358`). When set and headless, arm `store_deposit = true`
  on the final iteration (the same place `is_running` is cleared,
  `main.cpp:1064`), so the existing, quirk-preserved export block at
  `main.cpp:1177-1226` runs unchanged on the last frame. This follows
  the M2b precedent of port-added CLI flags; the F6 block itself is not
  touched.
- **Dataset-line fix:** `main.cpp:1191` writes the compile-time
  `DATASET_NAME` macro into `export_metadata.txt`. With the port's
  `--dataset` override, that would record the *wrong* dataset. Change it
  to write the effective `filename` (`main.cpp:391`). This preserves
  upstream *semantics* (upstream recorded the dataset actually loaded,
  because the macro was the only source) rather than upstream *letters*;
  `--dataset` is port infrastructure, so this is not a quirk violation.

No other `main.cpp` change. Notably the VAC sim parameters need **no code
edit**: the active `REGIME_SDSS` "SDSS large" constants (`main.cpp:63-69`:
sense 3.51 Mpc, persistence 0.89, sharpness 4.08) do NOT match the VAC
metadata (sense 4.6 / persistence 0.8 / sharpness 2.5) — see §7 for how
the run protocol handles this (a third constants block, following the
file's own alternate-block idiom at `main.cpp:55-69`).

## 5. Component: catalog converter — `tools/pack_vac_catalog.py`

New file. Mirrors `pack_data_celestial.py`'s output conventions (the
in-repo packer for the shipped catalog) so `main.cpp:398-409`'s
positional metadata parser accepts it.

Input: `~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv`
— 324,901 rows, no header (verified: row 1 is data), columns
`x,y,z,weight` with xyz in Mpc and weight in 10⁹ M☉
(validation-target-research §3, DATA_LINEAGE.md finding: CSV weights are
exactly 1000× the shipped bin's 10¹² M☉ convention).

Steps:

1. `np.loadtxt(..., delimiter=',')` → `(N,4)` float64, cast to float32
   at the end (matching upstream packer's `np.float32` output).
2. **Weight conversion: divide column 3 by 1000.0** (10⁹ → 10¹² M☉),
   restoring the convention `main.cpp:595`'s `log10f(1.0 + W)` load path
   expects.
3. **Sanity check (hard-fail):** mean converted weight must be within
   one order of magnitude of the shipped catalog's recorded
   `Mean weight = 0.013950215`
   (`bin/data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0_metadata.txt`)
   — same sample family, so a 1000× slip is unmistakable; also assert
   all weights > 0 and point count == 324,901.
4. Write `<out>.bin` (float32 XYZW, `arr.tofile`) and
   `<out>_metadata.txt` with the exact key sequence
   `pack_data_celestial.py:39-48` writes (`Number of points`, `Min X`,
   `Max X`, … `Mean weight`) — the parser is positional, order matters.
5. Default output path:
   `bin/data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0`
   (the VAC's own dataset name, per the reference `export_metadata.txt`).
   Add both files to `bin/data/SDSS/.gitignore` (which already ignores
   the `t=10.3` sibling) — ~5.2 MB of derived data stays out of git.

The run then uses `--dataset <that path>`; the compiled-in `t=10.3`
`DATASET_NAME` and the shipped 37k demo slice are untouched.

## 6. Grid anchoring + memory feasibility (task zero)

### 6.1 Anchoring to the exact VAC grid

Polyphorm auto-fits the grid from catalog bbox + padding
(`main.cpp:418-442`; quirk-preserved — **we do not fork this logic**).
The VAC grid was produced by this same algorithm (structural evidence:
multiple-of-8 dims, isotropic 0.78131 Mpc voxel —
validation-target-research §3), so the correct anchoring strategy is to
*feed it the same inputs*: the converted catalog's bbox and the right
`config.polyp` knobs.

`pack_vac_catalog.py --verify-grid` re-implements the fit formula
*read-only in Python* (bbox → pad by `GRID_PADDING × max-extent` → scale
`GRID_RESOLUTION` per axis → `nearest_multiple_of(·, 8)` → cubic-voxel
rescale of Y/Z world sizes) purely as a **predictor**; the C++ remains
authoritative and the run's own `export_metadata.txt` is the ground-truth
check. The verifier:

1. Computes the CSV bbox; checks midpoint ≈ (-239.469, -16.5618, 201.275)
   Mpc (the fit centers on the bbox midpoint, `main.cpp:431-433`, and
   padding doesn't move it — so this must match to sub-voxel or the
   catalog is not the VAC input and we stop).
2. Back-solves the padding from the VAC metadata:
   `p = (556.288 − extent_x) / max_extent`. If the catalog is truly the
   VAC input run at `GRID_PADDING = 0.1` (the current `bin/config.polyp:3`
   value), all three axes yield p ≈ 0.1 consistently — a free provenance
   check.
3. Predicts dims with `GRID_RESOLUTION = 1200` (config change from the
   current 1024, `bin/config.polyp:2`): expect exactly 712×1200×728 and
   world sizes matching 556.288×937.564×568.789 to float32 tolerance.

**If auto-fit misses** (dims or world size off): first remedy is the
back-solved `GRID_PADDING` value from step 2 written into `config.polyp`
— a documented config choice, no code change. Only if no single padding
value can reproduce the grid (not expected — the VAC provably came
through this formula) do we escalate to the human with the residual
misalignment quantified in voxels; forking the quirk-preserved fit logic
is a last resort requiring explicit sign-off, not a default.

Final verification is empirical either way: run `--headless 1 --dataset
<packed>`, read the printed grid line (`main.cpp:444-445`) /
`export_metadata.txt`, confirm 712×1200×728 before any long run.

### 6.2 Memory feasibility — explicit task zero

Native grid = 712×1200×728 ≈ 622M voxels; three r32float 3D textures
(trail A/B + trace) ≈ 7.5 GB, plus particle SoA (~10.3M × 6 f32 ≈
0.25 GB), display/splat targets, and export's transient ~3.7 GB
readback+pack peak. Before anything else in M5b:

1. `sysctl hw.memsize` — record the machine's unified-memory budget.
   Rule of thumb: need ≳ 16 GB total for the native-grid run + export
   headroom; Metal exposes most of physical RAM to a single process.
2. Headless allocation probe: `--headless 8 --dataset <packed>` with
   `GRID_RESOLUTION = 1200`. Dawn's error callbacks name the failing
   limit fatally at startup (spec's error-handling contract), so an
   8-iteration run either proves allocation + a sim step at full scale
   or fails loudly in seconds. (3D-texture dimension limit is not a
   concern: 1200 < Metal's 2048.)

**Documented fallback:** if allocation or paging makes native infeasible,
run at `GRID_RESOLUTION = 600` → predicted dims 356×600×364 (half scale;
verify with `--verify-grid --resolution 600`). Both cubes then
block-average to the common d8 grid — 356/89 = 600/150 = 364/91 = 4,
exact integer factors. **The comparison operates at d8 either way**; the
fallback costs simulation fidelity (coarser dynamics), not comparability,
and the report must state which resolution produced the numbers.

## 7. Run protocol

1. **Parameters.** The VAC metadata records: 10M agents, sense 4.6 Mpc,
   move 0.1 Mpc, sense spread 20°, move spread 10°, persistence 0.8,
   agent deposit 0, sampling sharpness 2.5. Sources in the port:
   - `config.polyp`: `Agents number = 10000000` already matches;
     `Grid resolution` 1024 → **1200** (§6.1); `Grid padding` per §6.1.
   - Compile-time `REGIME_SDSS` constants (`main.cpp:63-69`) do NOT
     match (3.51/0.89/4.08 vs 4.6/0.8/2.5). Add a third constants block
     "SDSS VAC" inside `#ifdef REGIME_SDSS` with the VAC values, active
     for the validation build, following the file's existing
     commented-alternates idiom (`main.cpp:55-69`). MOVE_DISTANCE 0.1 and
     spreads 20/10 already match. This is a data edit in upstream's own
     extension pattern, not logic change.
   - Post-run check: diff our `export_metadata.txt` against the reference
     one field-by-field (grid resolution/size/center, move/sense in both
     Mpc and vox, angles, persistence, deposit, sharpness). Any mismatch
     invalidates the run before comparison.
2. **Parameters the VAC metadata does NOT record — explicit unknowns:**
   iteration count at export, RNG seeding (upstream is unseeded anyway),
   `GRID_PADDING` (recoverable, §6.1), `HISTOGRAM_BASE` (stats-only, no
   sim effect), and any data-vs-agent deposit asymmetry internal to the
   shaders (ours is a faithful port of the producing code, so internal
   constants match by construction — but the *duration* knob is genuinely
   unrecorded). Treat iteration count as a protocol choice, documented
   with the results (risk §8).
3. **Duration / steady state.** Headless prints `E` every 50 iterations
   (`main.cpp:1474-1475`); the histogram-E plateau is the in-app
   convergence signal. Protocol: `--headless 1000 --export --dataset
   <packed>` (spec guidance "~1000+ iterations, energy plateau"). Accept
   as converged if relative ΔE over the final 200 iterations < 1%;
   otherwise rerun longer (e.g. 2000) until the plateau criterion holds.
   Record the E series alongside the export.
4. **Export.** `--export` (§4) fires the F6 block on the final frame:
   `export/trace.bin`, `export/deposit.bin`, `export/export_metadata.txt`,
   `export/halos_measurements.csv`. `trace.bin` is the validation
   artifact (the VAC published trace, not deposit).
5. **Compare.** §8 pipeline against
   `~/Development/js/skymap/data/raw/mcpm/mcpm_sdss_d8.npy`.

## 8. Component: comparison pipeline — `tools/validate/compare_trace.py`

Python 3 + numpy + scikit-image + matplotlib, offline, reads only files.
Conventions mirror `~/Development/vendor/python/PolyPhy/rhizome/tools/compareCubes.py`
(the sibling project's tool for this exact comparison —
validation-target-research §5) where sensible: per-axis max-projection
Pearson as first-class metrics, magma/log-scaled comparison render;
deviations from it (log10 transform, block-average instead of trilinear
zoom, masking variants) are deliberate and documented below.

CLI: `compare_trace.py --export-dir bin/export --reference
~/Development/js/skymap/data/raw/mcpm/mcpm_sdss_d8.npy --out <reportdir>
[--orientation-scan] [--eps 1e-3]`.

1. **Load ours.** Parse `export_metadata.txt` for `X x Y x Z` dims;
   `np.fromfile(trace.bin, np.float16).reshape(Z, Y, X)` — the .bin is
   headerless f16, Z-major/X-fastest (export-path-research §1/§6; axis
   order is implicit in the file, so the tool documents and pins it) —
   `.astype(np.float32)`, then `transpose(2, 1, 0)` → `(X, Y, Z)` to
   match the reference's `(712, 1200, 728)` = (X, Y, Z) convention from
   `extractMcpmCube.py`.
2. **Downsample to d8** with
   `skimage.transform.downscale_local_mean` — the *same operator* that
   produced the reference tiers (mean-preserving block average,
   validation-target-research §1/§2). Factors = our dims / (89, 150, 91);
   hard-fail unless exact integers (8 at native, 4 at half-res fallback).
   Pooling accumulates in float32 after the f16 load: f16 quantization
   (~1e-3 relative) already dominates any f32 accumulation error over
   ≤512-voxel blocks, so float64 buys nothing at 8× the memory.
3. **Log transform:** `log10(x + eps)`, default `eps = 1e-3`. Rationale
   (documented in-tool): d8 nonzero minimum is ≈ 3.9e-4
   (validation-target-research §1), so 1e-3 sits at the bottom of the
   signal range — no `-inf`, negligible compression of real structure,
   round number. No local precedent pins an eps (`compareCubes.py` is
   linear min-max), so this is a new, explicit choice; the report also
   prints results at eps 1e-4 and 1e-2 once as a sensitivity line.
4. **Masking — decision: report both, primary = joint support.**
   Primary metric masks to voxels where *both* cubes are nonzero
   pre-log ("exclude-zeros-in-either"). Why: ~70% of d8 reference voxels
   are exact zeros (§1 of the target research); including them adds a
   huge co-located constant-value mass at log10(eps) that inflates
   Pearson by rewarding agreement on empty background rather than
   structure — and the top-level spec (line 138) already specifies
   "over the joint support". The unmasked (all-voxel) number is also
   reported as a secondary diagnostic, since the PolyPhy precedent
   numbers are unmasked and comparability matters for interpreting our
   first measurement.
5. **Metrics:** Pearson (plain `np.corrcoef` on flattened arrays —
   equivalent to `compareCubes.py`'s manual formula) on the log cubes:
   (a) 3D voxelwise, masked + unmasked; (b) three axis max-projections —
   max-project the *linear* d8 cube along each axis, then
   `log10(+eps)`, then Pearson per axis (projection-first matches
   `compareCubes.py`'s structure-over-intensity rationale, lines 18-21).
   All eight numbers go in one table; **no pass/fail verdict is
   printed** — the tool reports, the human judges (user decision b).
6. **Orientation self-check (load-bearing, run once).** At d8 the dims
   (89, 150, 91) are pairwise distinct, so the *transposition* is pinned
   by shape alone — only one axis permutation of our cube even matches
   the reference shape. Reflections are not: `--orientation-scan`
   evaluates the three axis-projection Pearsons under all 8 flip
   combinations (±x, ±y, ±z) and prints the table. Procedure: run the
   scan once on the first real export, confirm identity (no flip) wins
   decisively, then pin that finding as a constant + comment in the tool
   (with the recorded table in the M5 results note) so routine runs skip
   the scan. If a flip wins, that is a bug in step 1's layout assumption
   — fix the transpose, don't ship a compensating flip.
7. **Sanity render:** side-by-side PNG per axis (ours vs reference max
   projections, `log10(+eps)`, magma colormap, shared color scale —
   `compareCubes.py` precedent) written to `--out`. A human eyeballs
   gross misalignment/mirroring before trusting any number.

## 9. Carryover triage (from m4b-carryovers.md)

| Item | M5? | Rationale |
|---|---|---|
| `save_texture3D` stub | **IN** | The milestone-critical blocker (§3). |
| Blend alpha-channel formula | OUT | Validation reads the exported cube, not pixels (spec line 182); rendering-only. |
| Palette RGBA8 UNORM-vs-SRGB gate | OUT | Rendering-only; pending human visual gate, orthogonal to export. |
| HUD/histogram fixed anchors | OUT | UI geometry; no bearing on export/validation. |
| DPI-only resize miss | OUT | Windowed-mode cosmetic; validation runs headless. |
| `get_panel_rect` latent assert | OUT | Zero call sites; M5 adds none. |
| `ps_volume_halocolor`/`velocity` wiring | OUT | Explicitly deferred by spec (line 33). |

## 10. Out of scope — explicit

- **OpenVDB converter** — post-M5 follow-up; M5 only keeps the f16 .bin
  friendly to it (headerless raw + metadata dims, §3).
- **F5 agents export beyond existing file-parity** — already works
  end-to-end via `capture_structured_buffer`
  (export-path-research §2); no M5 work.
- **halocolor / velocity analysis wiring** (`HALO_COLOR_ANALYSIS` /
  `VELOCITY_ANALYSIS` compile paths and their wider texture formats).
- **HUD anchors, DPI resize** (§9).
- **Setting the acceptance bar / quirk A/B hunts** — post-measurement,
  human-driven.
- **d2/d4 tier comparison** — d8 only in M5; coarser tiers are cheap
  follow-ups once the pipeline exists.

## 11. Task decomposition

**M5a — exporter (no dataset dependency, testable on the demo slice):**

1. Task zero: memory feasibility probe (§6.2) — record verdict + chosen
   `GRID_RESOLUTION` before committing to run scale.
2. `f32_to_f16` helper + CTest unit (known-vector table).
3. `save_texture3D` implementation (§3) + a headless smoke on the
   shipped 37k slice: `--headless 50 --export`, then assert
   `trace.bin` size == X·Y·Z·2 bytes from its own metadata and values
   round-trip as finite f16.
4. `--export` flag + dataset-line fix (§4).

**M5b — dataset, run, comparison (depends on M5a task 1 verdict):**

5. `tools/pack_vac_catalog.py` incl. `--verify-grid` (§5, §6.1); config
   + "SDSS VAC" constants block (§7.1).
6. Grid-anchor empirical check (`--headless 1`, confirm 712×1200×728 or
   documented fallback).
7. `tools/validate/compare_trace.py` (§8), developed against the
   reference cube plus a self-comparison smoke (reference vs itself → 1.0;
   reference vs shuffled → ~0).
8. Full protocol run (§7) + orientation scan + first measurement report
   (numbers, PNG, E series, config provenance) → human sets the bar.

M5a/M5b split is real: M5a is pure C++ with existing test
infrastructure; M5b is Python + a long GPU run, and its scale hinges on
task zero.

## 12. Risks

Carried honestly from validation-target-research §5:

1. **Precedent says the bar is low.** PolyPhy's reproduction with the
   *correct* input catalog, after 11 calibration runs fixing 4 real unit
   bugs, reached only ~0.085 3D / ~0.37-0.41 axis-projection Pearson
   (linear-normalized) vs this same reference. Our numbers may land in
   that neighborhood. Mitigations that make our attempt structurally
   stronger: we run a faithful port of the *exact producing code* (same
   quirks, same grid-fit, same f16 pipeline stage), on the anchored VAC
   grid, with log-trace metrics that don't collapse heavy-tailed data.
   But the measure-first bar exists precisely because "strong
   correlation" is unproven territory.
2. **Racy-deposit nondeterminism bounds attainable correlation.** The
   non-atomic accumulation is quirk-preserved; runs are statistically,
   never bitwise, reproducible (spec line 113-114). Two runs of *our own
   binary* will not correlate perfectly with each other — that
   self-correlation (worth measuring once as a ceiling estimate) bounds
   what any run can score against the reference. PolyPhy's two
   reproductions correlated worse with each other than with the
   reference (CALIBRATION.md) — a warning that stochastic spread can
   dominate at insufficient convergence.
3. **Unrecorded VAC parameters** (§7.2): iteration count foremost. If
   the published cube was exported far from our plateau criterion,
   morphology differs for protocol rather than correctness reasons.
   Documented as an interpretation caveat on the first measurement.
4. **Catalog surrogate gap.** `sample_3D_linW.csv` ≈ the VAC input
   within float32 noise and a 6% RSD-version position delta of median
   ~0.6 Mpc (DATA_LINEAGE.md via validation-target-research §3) — below
   the 0.78 Mpc voxel but nonzero; and its identity as "the actual VAC
   input" rests on rhizome's cross-match, not a byte-level check against
   the (unavailable) original. The §6.1 bbox-midpoint and back-solved
   padding checks are our independent verification; if they fail, stop
   and escalate.
5. **Memory** (§6.2): native grid + export transient may not fit smaller
   machines; the half-res fallback is defined and comparison-neutral at
   d8, but costs simulation fidelity — the report must name the
   resolution used.
6. **Axis-order mistakes are silent** without the §8.6 orientation scan;
   it runs before the first reported number, not after.
