#!/bin/sh
# Polyphorm.app main executable. Double-clicked from Finder it asks which
# dataset to load; run from a terminal with arguments it forwards them
# straight to the real binary, e.g.
#   Polyphorm.app/Contents/MacOS/Polyphorm --dataset /path/to/mydata --agents 2000000
# Output (Finder launches only) goes to ~/Polyphorm/polyphorm.log.
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/polyphorm-bin"
RES="$HERE/../Resources"
OUT="$HOME/Polyphorm"
AGENTS=1000000

# Old macOS versions pass a -psn_* process serial number on Finder launch.
case "$1" in -psn_*) shift ;; esac

if [ $# -gt 0 ]; then
    exec "$BIN" "$@"
fi

dialog() {
    osascript -e "display dialog \"$1\" with title \"Polyphorm\" buttons {\"OK\"} default button 1 with icon caution" >/dev/null 2>&1
}

# Never die silently on a Finder launch: say why if the log can't be written.
if ! { mkdir -p "$OUT" && : >>"$OUT/polyphorm.log"; } 2>/dev/null; then
    dialog "Polyphorm cannot write to $OUT (needed for its log and exports). Check that the folder is writable."
    exit 1
fi
exec >>"$OUT/polyphorm.log" 2>&1
echo "=== $(date) ==="

CHOICE=$(osascript <<'EOF'
set opts to {"2MRS — 34,974 galaxies (all-sky)", "SDSS — 324,901 galaxies (full)", "SDSS — 37,655 galaxies (quick slice)", "Open my own dataset…"}
set r to choose from list opts with title "Polyphorm" with prompt "Which dataset should Polyphorm load?" default items {item 1 of opts}
if r is false then return ""
return item 1 of r
EOF
)

case "$CHOICE" in
    2MRS*)          DATASET="data/2MRS/2mrs_gui" ;;
    "SDSS — 324"*)  DATASET="data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0" ;;
    "SDSS — 37"*)   DATASET="data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0" ;;
    Open*)
        PICKED=$(osascript -e 'POSIX path of (choose file with prompt "Choose a packed dataset (.bin or _metadata.txt) — see README for packing a CSV")' 2>/dev/null) || exit 0
        case "$PICKED" in
            *_metadata.txt) DATASET="${PICKED%_metadata.txt}" ;;
            *.bin)          DATASET="${PICKED%.bin}" ;;
            *) dialog "Please choose a .bin or _metadata.txt file produced by pack_catalog.py."; exit 1 ;;
        esac
        ;;
    *) exit 0 ;;
esac

case "$DATASET" in /*) CHECK="$DATASET" ;; *) CHECK="$RES/$DATASET" ;; esac
if [ ! -f "$CHECK.bin" ] || [ ! -f "${CHECK}_metadata.txt" ]; then
    dialog "Dataset not found: both $(basename "$CHECK").bin and $(basename "$CHECK")_metadata.txt are needed, in the same folder."
    exit 1
fi

echo "dataset: $DATASET"
"$BIN" --dataset "$DATASET" --agents "$AGENTS"
STATUS=$?
if [ $STATUS -ne 0 ]; then
    dialog "Polyphorm exited with an error (code $STATUS). Details are in ~/Polyphorm/polyphorm.log"
fi
exit $STATUS
