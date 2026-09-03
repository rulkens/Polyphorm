# 2MRS catalog

An extra dataset for interactive use, not part of the VAC validation.
All three files are tracked; `shasum -a 256 -c SHA256SUMS` checks them.

| File | Size | What it is |
|---|---|---|
| `2mrs_gui.csv` | 1.4 MB | 34,974 galaxies, no header, `x,y,z,weight`. Positions in equatorial cartesian Mpc, radial range 0.03 to 710 Mpc. Weights 0.1 to 3162, median 65; the unit is not recorded. |
| `2mrs_gui.bin`, `2mrs_gui_metadata.txt` | 560 KB | The CSV packed into Polyphorm's input format by `tools/pack_catalog.py` with the default weight scale of 1.0. Repacking reproduces the bin byte for byte. |

Launch with `./run_2mrs.sh`. The grid auto-fits to 1200x752x960 at
`config.polyp`'s resolution 1200, about 10 GB RSS at 10M agents.

## Provenance

The CSV is the input catalog of a PolyPhy GUI session on 2026-08-12
(the export sidecar `rhizome/data/output/2mrs_gui.json` on the
`rhizome-sdss-calibration` branch of <https://github.com/rulkens/PolyPhy>
records `N_DATA: 34974`, frame `equatorial-cartesian`, domain 1469 Mpc).
It was prepared from the 2MRS survey (Huchra et al. 2012) in the
maintainer's skymap catalog pipeline. The exact selection and weighting
steps that produced this CSV were not recorded, so unlike the files in
`bin/data/reference/` it cannot be re-derived from a public source; the
committed copy is the definition.

To repack after editing the CSV:

```sh
.venv/bin/python tools/pack_catalog.py --csv bin/data/2MRS/2mrs_gui.csv --out bin/data/2MRS/2mrs_gui
```
