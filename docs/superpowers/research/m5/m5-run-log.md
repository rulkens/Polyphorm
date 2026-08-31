# M5 run log — export + VAC validation

Working record for M5. Design: ../m5-export-validation-design.md (same dir).
Each task appends its section; nothing here is ever rewritten, only appended.

## Task zero: memory feasibility (design §6.2)

- `sysctl hw.memsize`: 68719476736 (64 GB)
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
- Chosen: GRID_RESOLUTION = 1200 (native)
- Empirical confirmation: deferred to the Task 7 allocation probe
  (`--headless` at full scale on the packed catalog; Dawn error callbacks
  name a failing limit fatally at startup, so it fails loudly in seconds
  if the verdict is wrong). 3D-texture dimension limit is not a concern:
  1200 < Metal's 2048.

## Task 7: grid anchor + allocation probe (design §6.1 final check, §6.2 probe)

- Command: `--headless 60 --export --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0`
- Grid line: `712 x 1200 x 728` (matches the published VAC grid; predictor agreed)
- Domain line: `556.29 x 937.56 x 568.79 Mpc`
- GRID_PADDING used: 0.1 (unchanged; no back-solve needed)
- Peak RSS (`/usr/bin/time -l`): 7,751,385,088 bytes maximum resident set
  size (peak memory footprint reported separately as 16,145,514,496 bytes)
- Export sizes: trace.bin = deposit.bin = 1,244,006,400 bytes (= X*Y*Z*2,
  712*1200*728*2)
- No `[gpu] uncaptured error`, no `FATAL`; run completed cleanly
  (`[gpu] device lost (2): Device was destroyed.` is the expected shutdown
  message after `--headless 60` finishes, not an error). Energy line:
  `E first=5.846246 last=23.046030 -> ENERGY RISING` (expected sim behavior,
  not a failure signal).
- Verdict: native-scale run + export CONFIRMED feasible (completes the Task
  zero probe). GRID_RESOLUTION = 1200 (native) stands; no fallback needed.

## Task 9: validation run 1 launched (protocol §7)

- Command: `--headless 1000 --export --dataset data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0`
- Log: run1-headless.log (same dir); launched 2026-08-13 01:29 local, git rev 41509ba
- config.polyp: Agents 10000000, Grid resolution 1200, Grid padding 0.1
- Compile-time constants: SDSS VAC (sense 4.6, persist 0.8, sharp 2.5,
  move 0.1, spreads 20/10, deposit 0)
- Known unrecorded-by-VAC parameters (design §7.2, interpretation caveats):
  iteration count at export (protocol choice: 1000 + plateau criterion),
  RNG seeding (upstream unseeded), GRID_PADDING (back-solved), HISTOGRAM_BASE
  (stats-only).
- Early sanity at ~2.5 min: process alive past the fatal-startup window
  (allocation/grid failures die in seconds — Task 7 probe verified the same
  binary+config produces grid 712x1200x728 and a clean export). Note: stdout
  is block-buffered to the log file, so iteration lines land in flushes; the
  E series is verified post-run (Task 10), not live.

## Task 10: convergence, metadata parity, orientation (run 1)

- Convergence: E(800) = 87.953613, E(1000) = 87.791565, relative dE = 0.1842% (< 1%: CONVERGED)
- Metadata diff vs reference export_metadata.txt: all fields PASS
  (data-point count 324901 vs 324849 — known surrogate delta, design risk 4)
- Orientation scan (design §8.6) — recorded table:
  ```
  orientation scan (8 flip combos, sorted by mean; identity must win):
    flips (x, y, z)             proj-x    proj-y    proj-z     mean
    (False, False, False)      +0.9873   +0.9849   +0.9903   +0.9875   <-- identity
    (False, True, False)       +0.9329   +0.9849   +0.8794   +0.9324
    (True, False, False)       +0.9873   +0.7807   +0.8502   +0.8727
    (True, True, False)        +0.9329   +0.7807   +0.8764   +0.8633
    (False, False, True)       +0.6542   +0.4897   +0.9903   +0.7114
    (False, True, True)        +0.6561   +0.4897   +0.8794   +0.6751
    (True, True, True)         +0.6561   +0.4656   +0.8764   +0.6661
    (True, False, True)        +0.6542   +0.4656   +0.8502   +0.6567
  ```
- Finding: identity wins by 0.0551 mean-Pearson over the best flip;
  PINNED_FLIPS stays (False, False, False).

## Task 11: first measurement (M5 deliverable — design §1)

Resolution of record: native 712x1200x728 (design §6.2). Iterations: 1000,
plateau dE = 0.1842%. Provenance: git rev 41509ba, SDSS VAC constants,
config.polyp resolution 1200 / padding 0.1, catalog = packed
sample_3D_linW.csv (324,901 pts).

### d8 log-trace Pearson (eps 1e-3)

| metric | masked (joint support) | unmasked |
|---|---|---|
| 3D voxelwise | +0.9640 | +0.9570 |
| x max-projection | +0.9869 | +0.9873 |
| y max-projection | +0.9833 | +0.9849 |
| z max-projection | +0.9828 | +0.9903 |

eps sensitivity: 1e-4 -> +0.9631 masked / +0.9406 unmasked; 1e-2 -> +0.9677
masked / +0.9733 unmasked.
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

Queued for presentation to the human partner 2026-08-13 (user AFK at
measurement time; presentation delivered in the session's wake-up summary):
the table above +
projections.png. Per the measure-first decision, the human sets the
acceptance bar now; misses against that bar trigger the spec's
quirk-by-quirk A/B hunts (post-M5). Human's decision (2026-08-13, verbatim):
"accept at ≥0.9" — the measured 3D log-trace Pearson at d8 (+0.9640 masked /
+0.9570 unmasked) clears the accepted bar. M5 PASSED; milestone closed.
No quirk A/B hunts triggered.
