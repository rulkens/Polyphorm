#!/bin/sh
# Launch the Polyphorm GUI on the SDSS data.
# Default: full VAC catalog (324,901 galaxies). Pass --quick for the
# smaller upstream viz slice (37,655 galaxies). Extra args are forwarded
# to the binary. Note the 1M-agent start below also applies to headless
# runs; for the validation protocol invoke the binary directly (see
# docs/RUNNING.md, "Validation against the published VAC").
set -e
cd "$(dirname "$0")/bin"

DATASET="data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0"
if [ "$1" = "--quick" ]; then
    DATASET="data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0"
    shift
fi

[ -x ../build/polyphorm ] || { echo "error: build/polyphorm not found — build first (cmake --build build -j 8)" >&2; exit 1; }
[ -f "$DATASET.bin" ] || { echo "error: $DATASET.bin missing — see docs/RUNNING.md (Datasets)" >&2; exit 1; }

# Start at 1M active agents (GUI dropdown can still scale up to the
# config.polyp maximum). A later --agents in "$@" overrides this.
exec ../build/polyphorm --dataset "$DATASET" --agents 1000000 "$@"
