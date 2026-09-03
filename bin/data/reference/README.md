# Validation reference data

Everything the validation pipeline needs that did not originate in this
repository. All three committed files are byte-identical to their public
sources; `SHA256SUMS` pins them and `shasum -a 256 -c SHA256SUMS` checks
them.

| File | Size | What it is | Public source |
|---|---|---|---|
| `sample_3D_linW.csv` | 16 MB | Input galaxy catalog: 324,901 rows, no header, `x,y,z,weight`, positions in Mpc, weights in 10^9 Msun. The catalog the published VAC was run on (see Lineage). | [PolyPhyHub/PolyPhy `data/csv/sample_3D_linW.csv`](https://github.com/PolyPhyHub/PolyPhy/blob/main/data/csv/sample_3D_linW.csv), git blob `2ff3a3b84e743d01260169816ca9184662a80b9c` |
| `export_metadata.txt` | 544 B | The VAC's own export metadata: grid 712x1200x728, domain, center, and the MCPM parameters the published cube was produced with. | SDSS SAS (below), official sha1 `41033bc85675361a950c6f2ac83020ddafd093ac` |
| `mcpm_sdss_d8.npy` | 4.6 MB | The published trace cube block-averaged 8x to (89, 150, 91) float32, axis order (X, Y, Z). The comparison target. | Derived from the SAS cube by `tools/validate/extract_reference.py` (below). Mirror: `https://skymap-data.rulkens.com/data/raw/mcpm/mcpm_sdss_d8.npy` |

Not committed, but placed here by the tools:

| File | Size | Produced by |
|---|---|---|
| `trace.bin.bz2` | 345 MB | `tools/validate/download_vac.py`, from the SDSS archive |
| `trace.bin` | 2.3 GB | `tools/validate/extract_reference.py` decompresses it on first run |
| `mcpm_sdss_d4.npy`, `mcpm_sdss_d2.npy` | 37 MB, 296 MB | `extract_reference.py --factors 8 4 2` (not needed for validation) |
| `mcpm_v1_0_0_datacube_SDSS_z_44-476mpc.sha1sum` | | the archive's own checksum file, fetched by the download script |

## The published cube

SDSS DR17 value-added catalog "Cosmic web environmental densities from
MCPM slime mold", Wilde, Burchett, Elek et al. 2023
([arXiv:2301.02719](https://arxiv.org/abs/2301.02719)). Landing page:
<https://www.sdss4.org/dr17/data_access/value-added-catalogs/?vac_id=cosmic-web-environmental-densities-from-mcpm-slimemold>

Archive directory:
<https://data.sdss.org/sas/dr17/env/EBOSS_LSS/mcpm/v1_0_1/datacube/SDSS_z_44-476mpc/>

It holds `trace.bin.bz2` (official sha1
`0572af99f3789bafca5b39a84824458500f9b4c2`), `export_metadata.txt`, and the
`.sha1sum` file. The uncompressed `trace.bin` is headerless float32,
712x1200x728, Z-major / X-fastest. Note the published cube is float32
while this port's own exports are float16; the comparison tool
upcasts both to float32 before anything else.

## Regenerating the d8 cube

```sh
.venv/bin/python tools/validate/download_vac.py        # 345 MB, verifies sha1
.venv/bin/python tools/validate/extract_reference.py   # writes mcpm_sdss_d8.npy, checks SHA256SUMS
```

`extract_reference.py` is the script that produced the committed file,
copied from the skymap repository (`tools/volumes/extractMcpmCube.py` at
commit `c78c4e0d35624d05dd4fa906109f8fad9dbdb567`) with paths made
repo-relative. It reads the cube through `pyslime`, the library the VAC
authors recommend, and block-averages with
`skimage.transform.downscale_local_mean`. The comparison tool downsamples
this port's export with the same function, so both sides pass through
the same operator.

## Lineage of the input catalog

The VAC's `export_metadata.txt` names its input
`sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0` with 324,849
points. That file was never published. `sample_3D_linW.csv`, bundled
with PolyPhy since its 2022 OSPO workshop checkpoint (commit
`29e8904f95f2e63e22472727e9cf83a30b177407`), has 324,901 points over the
same 45 to 475 Mpc radial range.

The identification rests on a KD-tree cross-match done in the rhizome
calibration work (`rhizome/docs/DATA_LINEAGE.md` on the
`rhizome-sdss-calibration` branch of <https://github.com/rulkens/PolyPhy>):
the 37,655-galaxy viz slice shipped with upstream Polyphorm is the
66 to 149 Mpc shell of this CSV, with 94% of positions matching
bit-exactly and the remaining 6% differing by a median of 0.6 Mpc
(below the 0.78 Mpc voxel), and weights exactly 1000x apart (10^9 vs
10^12 Msun). The packer `tools/pack_vac_catalog.py` divides the weights
by 1000 and hard-fails on the point count and on a mean-weight unit slip.

Independent checks that this repo performs on the catalog:
`pack_vac_catalog.py --verify-grid` reproduces the VAC's grid center to
sub-voxel precision from the CSV's bounding box, and back-solves the
grid padding to 0.1 on all three axes. The 52-point count difference and
the 6% position delta are documented interpretation caveats in
`docs/superpowers/research/m5/m5-run-log.md`.
