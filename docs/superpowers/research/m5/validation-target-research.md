# M5 Validation Target Research

Researched read-only. All claims sourced from local files only (no network). Paths are absolute where load-bearing.

## 1. Reference cubes: `~/Development/js/skymap/data/raw/mcpm/`

Directory listing:

| File | Size |
|---|---|
| `export_metadata.txt` | 544 B |
| `mcpm_sdss_d2.json` | 462 B (hand-written v1 sidecar, see §2) |
| `mcpm_sdss_d2.npy` | 311,001,728 B (~296 MB) |
| `mcpm_sdss_d4.npy` | 38,875,328 B (~37 MB) |
| `mcpm_sdss_d8.npy` | 4,859,528 B (~4.6 MB) |
| `README.md` | 2,422 B |
| `trace.bin` | 2,488,012,800 B (~2.3 GB, uncompressed native cube) |
| `trace.bin.bz2` | 344,985,894 B (~345 MB, upstream compressed distribution) |

Loaded each `.npy` with `numpy.load` (mmap then full-array stats). Results:

| Tier | dtype | shape | min | max | mean | frac zero | p50/p90/p99/p99.9 | nonzero min |
|---|---|---|---|---|---|---|---|---|
| d2 | float32 | (356, 600, 364) | 0.0 | 81543.55 | 15.9563 | 78.8% | 0 / 7.24 / 352.4 / 2096.9 | 0.00315 |
| d4 | float32 | (178, 300, 182) | 0.0 | 40429.77 | 15.9563 | 73.4% | 0 / 9.18 / 385.0 / 1635.5 | 0.000393 |
| d8 | float32 | (89, 150, 91)   | 0.0 | 12616.52 | 15.9563 | 69.9% | 0 / 18.9 / 351.99 / 1120.0 | 0.000394 |

Mean is preserved exactly across tiers (15.9563...) — confirms **mean-preserving block-average downsampling**, not stride/nearest-neighbour decimation. Zero-fraction falls as tiers coarsen (78.8% → 73.4% → 69.9%), consistent with block averaging pulling exact-zero void voxels up to small positive values as neighboring nonzero cells get folded in.

Values are **linear trace density**, not pre-log-scaled: heavy-tailed distribution spanning ~5 decades (min nonzero ≈ 3e-4 to max ≈ 8e4), 70-79% exact zeros (background/void), a handful of orders of magnitude between p50 (0) and p99.9. Confirmed independently by `tools/utils/volume/packLogTraceVoxels.ts` (skymap repo), which applies `log(1+v)/log(1+max)` explicitly *because* the source `.npy` is linear and heavy-tailed ("a linear map put 99% of voxels under contrast's first click" — packLogTraceVoxels.ts:9-10).

**d2/d4/d8 meaning:** block-average downsample factors 2×, 4×, 8× of a parent grid. Parent (native) resolution is **712 × 1200 × 728** voxels (confirmed by `export_metadata.txt` and `tools/volumes/extractMcpmCube.py:6,54`). d2 = 712/2 × 1200/2 × 728/2 = 356×600×364 ✓; d4 = 178×300×182 ✓; d8 = 89×150×91 ✓ (all exact integer divisions — 712/1200/728 are each divisible by 8).

## 2. Provenance

Full chain, cited:

1. **Upstream source:** SDSS Science Archive Server, `https://data.sdss.org/sas/dr17/env/EBOSS_LSS/mcpm/v1_0_1/datacube/SDSS_z_44-476mpc/trace.bin.bz2` — the published **SDSS DR17 Cosmic Slime VAC** (Wilde, Burchett, Elek et al. 2023, arXiv:2301.02719). Cited in `~/Development/js/skymap/data/raw/mcpm/README.md:36-37` and `tools/volumes/extractMcpmCube.py:69`.
2. **Extraction:** `~/Development/js/skymap/tools/volumes/extractMcpmCube.py` — one-shot maintainer script. Decompresses `trace.bin.bz2` → `trace.bin`, parses via the upstream-recommended Python library `pyslime` (`pip install pyslime`) into a `(712, 1200, 728)` float32 array, reads `export_metadata.txt` for grid dims, block-averages by factors {8, 4, 2} via `skimage.transform.downscale_local_mean`, writes the three `.npy` tiers. This is a **direct extraction of the published VAC** — not a local Polyphorm/PolyPhy run.
3. **Design record:** `~/Development/js/skymap/docs/superpowers/specs/completed/2026-05-11-mcpm-cosmic-web-volume-design.md` — full design doc; explicitly lists "Running MCPM ourselves (PolyPhy / Polyphorm route)" as **out of scope** for this ingest (line 26). Confirms the `.npy` tiers are a pass-through of the published cube, packaged for skymap's WebGPU volume renderer (`mcpm-{small,medium,large}.scfd`).
4. **`mcpm_sdss_d2.json` sidecar** (skymap/data/raw/mcpm/mcpm_sdss_d2.json) is explicitly a *hand-written, non-authoritative* note ("DoD manual pass") — not part of the extraction provenance; ignore its numeric fields as authoritative metadata, they're a manual restatement of `buildMcpmVolume.ts` constants.

**Verdict: the reference cubes are extracted directly from the published VAC's `trace.bin`, not produced by any local Polyphorm run.**

## 3. Input catalog match — the shipped Polyphorm bin is a small subset, not the VAC's actual input

`export_metadata.txt` (skymap/data/raw/mcpm/, which is the *actual upstream* metadata file used to build the VAC's `trace.bin`) states:

```
dataset: data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0
number of data points: 324849
number of agents: 10M
simulation grid resolution: 712 x 1200 x 728 [vox]
simulation grid size: 556.288 x 937.564 x 568.789 [mpc]
simulation grid center: (-239.469, -16.5618, 201.275) [mpc]
```

The Polyphorm repo's shipped input is `bin/data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0_metadata.txt`:

```
Number of points = 37655
Min/Max X = -148.27 / -7.07     (range 141.2 Mpc)
Min/Max Y = -143.37 / 139.93    (range 283.3 Mpc)
Min/Max Z = -11.80 / 136.48     (range 148.3 Mpc)
Mean weight = 0.013950215
```

These are **different dataset names, different point counts (37,655 vs 324,849 — ~8.6× fewer), different provenance labels**. A naive bounding-box check makes them look unrelated: the reference grid's X-range is `[grid_center_x ± size_x/2] = [-517.61, -38.33]` Mpc, but the shipped catalog's max X (-7.07) lies *outside* that box on the near side — because the reference box is off-center relative to the observer (who sits at the origin), while the shipped catalog is a spherical radial shell around the observer, so a shell's near edge can poke past an off-center box face.

**However, I found an independent, more rigorous cross-repo lineage analysis that resolves this precisely:** `~/Development/vendor/python/PolyPhy/rhizome/docs/DATA_LINEAGE.md` (dated 2026-08-10, produced for a related PolyPhy-based reproduction effort — see §5) did a KD-tree empirical cross-match of three catalogs:

| Dataset | Points | Radial extent | Notes |
|---|---|---|---|
| Polyphorm `bin/data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0.bin` | 37,655 | 66–149 Mpc | this repo's shipped input |
| PolyPhy `data/csv/sample_3D_linW.csv` (bundled with PolyPhy, = the actual Wilde et al. VAC input) | 324,901 | 45–475 Mpc | near-identical count to export_metadata's 324,849 |
| skymap `sdss-large.bin` | 498,208 | 4–1190 Mpc | different (comoving, H0=70) catalog family |

Finding (DATA_LINEAGE.md:17-22): **"Polyphorm bin ⊂ PolyPhy CSV. Same catalog: identical angular footprint; the bin is the 66–149 Mpc radial shell of the CSV. 94% of positions match bit-exactly; the remaining 6% differ by ~0.6 Mpc (median) — later catalog version with RSD-corrected positions (`rsdCorr` lineage). Matched weights are exactly 1000× (CSV in 10⁹, bin in 10¹² M☉); the mass values themselves are identical."**

So: the shipped `galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0.bin` **is genealogically the same underlying galaxy sample** as the real VAC input (`sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0`), but it is **only the 66–149 Mpc radial shell** of it — roughly an 8.6× smaller, radially truncated slice of the full 44–476 Mpc volume the published cube was built from. Corroborating evidence found independently in this repo:

- `bin/data/SDSS/.gitignore` lists `/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=10.3.bin` (and its metadata) — the *real* dataset name is known to this repo but the file itself is gitignored/not present locally.
- `main.cpp:48-51` (under `#ifdef REGIME_SDSS`) has three `DATASET_NAME` candidates, two commented out (`galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0`, `galaxiesInSdssSlice_viz_huge_t=10.3`) and one **active**: `"data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=10.3"` — i.e., the currently-wired dataset name matches the VAC's real input family, but at `t=10.3` (a mass/luminosity threshold parameter — see `pack_data_celestial.py:6,32`, `t` = `mass_threshold`, not simulation time), not the VAC's `t=0.0`. That file is not present in the repo either.
- `pack_data_celestial.py` is the generator for the *shipped* `galaxiesInSdssSlice_viz_bigger_lumdist` catalog only (converts a `.dat` with RA/Dec/dist/logmass columns to xyz+mass `.bin`); it has no knowledge of the `sdssGalaxy_rsdCorr_dbscan...` pipeline (DBSCAN dedup + RSD correction), confirming that catalog was produced by a separate, more sophisticated upstream pipeline not present in this repo.

**Grid-resolution corroboration (Polyphorm's own algorithm, `main.cpp:420-442`):** `WORLD_SIZE_{X,Y,Z}` = catalog bbox extent + `GRID_PADDING × WORLD_SIZE_MAX`; then `GRID_RESOLUTION_{X,Y,Z} = nearest_multiple_of(GRID_RESOLUTION × WORLD_SIZE_axis/WORLD_SIZE_MAX, 8)`. This produces an **isotropic voxel edge across all three axes, with each axis dimension forced to a multiple of 8** — exactly the pattern seen in the reference cube (712/1200/728 are all multiples of 8; voxel edge 0.78131 Mpc is uniform on all three axes: 556.288/712 = 937.564/1200 = 568.789/728 = 0.78131). This is strong structural evidence the reference cube's grid was produced by this exact algorithm (Polyphorm or a PolyPhy sibling sharing the convention) — with an effective `GRID_RESOLUTION` config value of **~1200** (the max/Y-axis resolution), not the **1024** currently in `bin/config.polyp:2`. Agent count also matches: `bin/config.polyp:1` has `Agents number = 10000000`, and `export_metadata.txt` records `number of agents: 10M` — consistent.

**Confidence: high that the shipped catalog is NOT sufficient to reproduce the reference cube** (it's a small radial subset, ~66-149 Mpc of a 44-476 Mpc volume — most of the reference cube's volume would be empty if simulated from the shipped catalog alone). **Confidence: high (via the DATA_LINEAGE.md cross-match, not independently re-verified here) that the shipped catalog is a genuine radial-shell child of the same underlying galaxy sample** used for the VAC. **Unverifiable locally:** the full `sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0` catalog (324,849 pts) is not present in this repo or in skymap; the nearest available surrogate is PolyPhy's bundled `data/csv/sample_3D_linW.csv` (324,901 pts) at `~/Development/vendor/python/PolyPhy/data/csv/sample_3D_linW.csv`, whose provenance as "the actual Wilde 2023 VAC input" is asserted by `rhizome/README.md:67` but not re-verified by me byte-for-byte against `export_metadata.txt`'s 324,849.

## 4. Published VAC context (local docs only)

No local FITS/README from the SAS directory itself is present (`README.txt` from the SAS is referenced but not shipped locally; only `export_metadata.txt` is present in `~/Development/js/skymap/data/raw/mcpm/`). What's documented locally, all from `~/Development/js/skymap/docs/superpowers/specs/completed/2026-05-11-mcpm-cosmic-web-volume-design.md`:

- Grid: 712×1200×728 voxels, 556.288×937.564×568.789 Mpc, center (-239.469, -16.5618, 201.275) Mpc, base voxel edge ≈0.7813 Mpc.
- Format: **not FITS** — Polyphorm's native binary "trace" format, read via `pyslime`.
- Frame: equatorial-cartesian comoving Mpc, observer at origin (same frame as SDSS spectroscopic positions).
- Units/scaling: **the design doc does not state the VAC stores log-scaled data** — it explicitly treats the raw values as linear and log-normalizes downstream itself (`packLogTraceVoxels.ts`), matching the empirical stats in §1 (heavy-tailed, min=0, not already log-compressed).
- No local documentation states any smoothing/downsampling applied by Wilde et al. themselves before publishing `trace.bin` — the d2/d4/d8 downsampling in `extractMcpmCube.py` is skymap's own, done *after* download, not part of the published product.
- Input dataset for the VAC: `sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0` — 324,849 SDSS galaxies, redshift-space-distortion corrected, DBSCAN-deduplicated (eps=2.0, minSamples=3), redshift bin width 0.001, mass cut M*>10^10 M☉ (per `~/Development/vendor/python/PolyPhy/rhizome/docs/DATA_LINEAGE.md:52-57`, sourced to Wilde et al. 2023 and Burchett et al. 2020/2016 for the mass-weighting scheme: stellar mass weighted directly, no halo-mass conversion for SDSS).
- **I found no local doc that states an explicit smoothing kernel or filter beyond the MCPM agent-simulation process itself.** If such details exist they would be in the SAS `README.txt`, which is not present locally — I did not guess.

## 5. Comparison methodology sketch

**Precedent exists** for this exact comparison, in a sibling project (`~/Development/vendor/python/PolyPhy`, fork `rulkens/PolyPhy` branch `rhizome-sdss-calibration`, present locally) that attempted to *reproduce* this same reference cube by running a Python/Taichi MCPM (PolyPhy) on skymap's SDSS catalog. Its comparison tool is `~/Development/vendor/python/PolyPhy/rhizome/tools/compareCubes.py`:

- Loads two 3D float32 `.npy` cubes.
- **Normalizes each independently via min-max to [0,1]** (`normalise_minmax`) — no log transform in this tool.
- Resamples the smaller cube up to the larger's shape via `scipy.ndimage.zoom(order=1)` (trilinear) when shapes differ.
- Computes Pearson via manual dot-product formula (`correlation_score`) — both a full 3D voxelwise score and three 2D max-projection scores (one per axis), the latter explicitly favored because "MCPM trace doesn't carry consistent absolute intensity across the depth axis" and 2D projections are "much less sensitive to small voxel-grid misalignments than 3D Pearson" (`compareCubes.py:18-21`).
- Renders a magma/LogNorm comparison PNG for visual sanity-check.

**Results from that effort are directly relevant risk data for M5**, documented in `rhizome/docs/CALIBRATION.md` (11 runs, `runs/001`–`runs/011`, each with a full `manifest.json`): even the *actual* VAC input catalog (`vac_input`, PolyPhy's bundled `sample_3D_linW.csv`, ≈324,901 pts) run through PolyPhy against the reference (`mcpm_sdss_d2.npy`) only reached **3D Pearson ≈ 0.085 and axis-projection Pearson ≈ 0.37–0.41** at the latest run (`011-input-v9b-refresh/manifest.json`), on **linear min-max normalized** data (not log-trace). The project's own provisional acceptance bar is **≥0.6 on two of three axis projections** — explicitly *lower* than M5's ≥0.9 target — and after 11 tuning iterations (fixing 4 real unit-conversion bugs: Mpc-vs-pixel sense/step distances, degrees-vs-radians sensing angle, 1M-vs-10M agent count, and non-zero agent self-deposit) the best `skymap_sdss vs reference` result is still only **+0.43/+0.45/+0.45** axis projections (`CALIBRATION.md`'s results table, run 011). `CALIBRATION.md:209-220` notes the two independently-run reproductions (`skymap_sdss` and `vac_input`) correlate *less* with each other than either does with the reference, attributed to PolyPhy's **unseeded stochastic RNG** dominating signal at these iteration counts.

This precedent is a different codebase (Python/Taichi, not this repo's C++/Metal port) and a different comparison convention (linear, not log-trace), so its numbers don't transfer directly — but it demonstrates concretely that (a) getting close to the reference via re-simulation is hard even with the correct input catalog, (b) 3D voxelwise Pearson is far noisier/lower than axis-projection Pearson for this kind of heavy-tailed, stochastic-agent field, and (c) log-transforming should meaningfully help (M5's own log-trace convention likely exists precisely because raw-linear correlation crushes toward zero — matches what `packLogTraceVoxels.ts` says about linear maps collapsing contrast for this exact data).

**Concrete proposal for M5's exporter + comparison, given d8's actual dtype/scale:**

1. **Exporter output:** dump the port's own trace field as float32 (matching the reference's dtype), same axis convention Polyphorm already uses internally (C-order X-slowest per `main.cpp`'s texture layout — verify against whatever the port's `Texture3D` readback order is; don't assume it matches skymap's WebGPU x-fastest transpose, that's a skymap-specific convention for `packLogTraceVoxels.ts`, not universal).
2. **Downsampling to d8:** **mean-pool (block-average), not stride/nearest-neighbour** — this is what produced the reference's own d8 tier (`extractMcpmCube.py`'s `skimage.transform.downscale_local_mean`), confirmed mean-preserving in §1's stats. Using stride decimation would alias and bias the comparison unfairly against the port. If the port's own simulation resolution doesn't cleanly factor to 89×150×91, resample (trilinear, `scipy.ndimage.zoom(order=1)`, matching `compareCubes.py`'s and PolyPhy's own convention) rather than block-average a non-integer factor.
3. **Masking:** given 70% of d8 reference voxels are exact zero (background/void, §1), decide explicitly whether to include them in the Pearson sum. Including them will inflate correlation somewhat (both cubes agreeing on "empty" background) — the design doc should state a choice; excluding zero-in-either-cube voxels is a defensible "structure-focused" alternative worth specifying explicitly rather than leaving implicit.
4. **Log transform:** `log10(x + eps)`. Given nonzero minimums around 3e-4 (d2/d4) to 4e-4 (d8), an eps in that neighborhood (e.g. `1e-3` or the tier's own nonzero-min) avoids `log10(0)=-inf` while not swamping the signal; alternatively mirror skymap's own convention, `log1p` (`log(1+v)`, natural log) used in `packLogTraceVoxels.ts:41` — but note M5's stated methodology is `log10`, so pick `log10(x + eps)` and document the eps choice explicitly (this wasn't pinned by any local precedent — `compareCubes.py` uses linear min-max, not log, so there's no existing eps convention to mirror for log10 specifically).
5. **Pearson:** compute on the log-transformed, downsampled, (optionally masked) d8 cubes — 3D voxelwise, and consider reporting axis-projection Pearson as a supplementary/diagnostic metric per the `compareCubes.py` precedent, since 3D voxelwise Pearson proved far noisier in the sibling project's own results.
6. **Grid alignment is load-bearing:** given §3's finding that grid-fitting (bbox anchoring to the VAC's exact center/extent) was itself a discovered bug in the PolyPhy effort (`CALIBRATION.md`'s "Bbox match" section — "Without bbox alignment, voxel-space Pearson comparison compares different physical regions and collapses to noise"), the design doc must specify how the port's simulation grid will be anchored to the reference's exact `(-239.469, -16.5618, 201.275)` Mpc center and `556.288×937.564×568.789` Mpc extent — this cannot be left to auto-fit from whatever input catalog subset is actually used.

**Biggest open risk for the design doc to address explicitly:** the shipped input catalog is a small radial subset (§3) of what generated the reference; a from-scratch re-simulation with the *correct* full catalog, by a closely related implementation, still only reached ~0.4 axis-projection Pearson on linear data after 11 tuning iterations. Whether log-transformed 3D Pearson on d8 with the *shipped* (subset) catalog can plausibly clear 0.9 is questionable and should be flagged as a milestone risk, not assumed achievable.
