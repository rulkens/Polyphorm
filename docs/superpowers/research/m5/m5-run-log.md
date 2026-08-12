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
