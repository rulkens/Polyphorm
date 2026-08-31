#!/bin/sh
# Launch the Polyphorm GUI on the 2MRS catalog (34,974 galaxies,
# equatorial cartesian Mpc; grid auto-fits to 1200x752x960, ~10 GB RSS).
# Extra args are forwarded to the binary (e.g. --headless 1000 --export).
set -e
cd "$(dirname "$0")/bin"

DATASET="data/2MRS/2mrs_gui"

[ -x ../build/polyphorm ] || { echo "error: build/polyphorm not found — build first (cmake --build build -j 8)" >&2; exit 1; }
[ -f "$DATASET.bin" ] || { echo "error: $DATASET.bin missing — regenerate with tools/pack_catalog.py" >&2; exit 1; }

# Start at 1M active agents (GUI dropdown can still scale up to the
# config.polyp maximum). A later --agents in "$@" overrides this.
exec ../build/polyphorm --dataset "$DATASET" --agents 1000000 "$@"
