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
