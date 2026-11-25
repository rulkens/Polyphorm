# SDSS catalogs

| File | Size | What it is |
|---|---|---|
| `galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0` `.bin`, `_metadata.txt` | 600 KB | The 37,655 galaxy viz slice shipped with upstream Polyphorm ("SDSS quick slice"). Tracked. |
| `sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0` `.bin`, `_metadata.txt` | 5.2 MB | The full VAC catalog, 324,901 galaxies ("SDSS full"). Not tracked; `tools/pack_vac_catalog.py` packs it from `bin/data/reference/sample_3D_linW.csv`. |
| `sdss_vac_1e12Msun.csv` | 17 MB | The full VAC catalog as a CSV in 10^12 Msun: 324,901 rows, no header, `x,y,z,weight`. Tracked; `shasum -a 256 -c SHA256SUMS` checks it. |

## Which catalog file is which

Polyphorm deposits `log10(1 + weight)` per galaxy, so the weight unit
changes the result. Every dataset this port runs is in stellar mass in
10^12 Msun. The published catalog CSV is in 10^9 Msun and needs a weight
scale of 0.001; the copy here does not.

| File | Unit | Weight scale to load it | What it is |
|---|---|---|---|
| `bin/data/reference/sample_3D_linW.csv` | 10^9 Msun | 0.001 | The published catalog file, unchanged |
| `bin/data/SDSS/sdss_vac_1e12Msun.csv` | 10^12 Msun | 1 | The same rows, weights / 1000 |
| `bin/data/SDSS/sdssGalaxy_rsdCorr_..._t=0.0` `.bin` | 10^12 Msun | (packed) | What the app loads as "SDSS full" |
| `bin/data/2MRS/2mrs_gui.csv` and `.bin` | 10^12 Msun | 1 | 2MRS, converted in v1.2.1-alpha |

## How `sdss_vac_1e12Msun.csv` was made

From `bin/data/reference/sample_3D_linW.csv`, row by row in the same
order: x, y and z copied as text, byte for byte, and the weight divided
by 1000 as a decimal shift of the text (Python `decimal.Decimal`, no
float rounding), so every weight here times 1000 equals the published
one exactly. 33 weights that the published file writes in exponent form
(`9.80E-05`) are written out in full (`0.0000000980`). The published
file itself is untouched; its provenance and lineage are in
`bin/data/reference/README.md`.

`dataset_tests` checks that this CSV at weight scale 1 loads
bit-identically to the packed "SDSS full" pair, as the published CSV
does at 0.001:

```sh
cd bin
../build/polyphorm --dataset data/SDSS/sdss_vac_1e12Msun.csv --agents 1000000
```
