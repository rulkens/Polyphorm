# 2MRS catalog

An extra dataset for interactive use, not part of the VAC validation.
All three files are tracked; `shasum -a 256 -c SHA256SUMS` checks them.

| File | Size | What it is |
|---|---|---|
| `2mrs_gui.csv` | 1.4 MB | 34,974 galaxies, no header, `x,y,z,weight`. Positions in equatorial cartesian Mpc, radial range 0.03 to 710 Mpc. Weights are stellar mass in 10^12 Msun, 0.0001 to 3.162, median 0.065. |
| `2mrs_gui.bin`, `2mrs_gui_metadata.txt` | 560 KB | The CSV packed into Polyphorm's input format by `tools/pack_catalog.py` with the default weight scale of 1.0, so the pair is in 10^12 Msun too. Repacking reproduces the bin byte for byte. |

Launch with `./run_2mrs.sh`. The grid auto-fits to 1200x752x960 at
`config.polyp`'s resolution 1200, about 10 GB RSS at 10M agents.

## Weight unit

The CSV and the packed pair are both stellar mass in 10^12 Msun, the
unit of the SDSS datasets. It matters because Polyphorm deposits
`log10(1 + weight)` per galaxy, so the unit changes the result. Up to
v1.2.0-alpha this file was in 10^9 Msun (0.1 to 3162, mean 85.6), which
put 2MRS runs on the flattened part of that curve. That unit was not
recorded anywhere; it was inferred from the weight range, whose ends are
exactly the clamp limits 10^8 and 10^12.5 Msun of the pipeline that made
the file, at mass / 10^9. The weights were divided by 1000 for
v1.2.1-alpha as a decimal shift of the text, positions untouched (mean
now 0.0856).

## Provenance

The CSV is the input catalog of a PolyPhy GUI session on 2026-08-12
(the export sidecar `rhizome/data/output/2mrs_gui.json` on the
`rhizome-sdss-calibration` branch of <https://github.com/rulkens/PolyPhy>
records `N_DATA: 34974`, frame `equatorial-cartesian`, domain 1469 Mpc),
with the weights divided by 1000 as described above.
It was prepared from the 2MRS survey (Huchra et al. 2012) in the
maintainer's skymap catalog pipeline. The exact selection and weighting
steps that produced this CSV were not recorded, so unlike the files in
`bin/data/reference/` it cannot be re-derived from a public source; the
committed copy is the definition.

To repack after editing the CSV:

```sh
.venv/bin/python tools/pack_catalog.py --csv bin/data/2MRS/2mrs_gui.csv --out bin/data/2MRS/2mrs_gui
```
