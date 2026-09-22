#!/bin/sh
# Polyphorm.app main executable. Double-clicked from Finder it asks which
# dataset to load (and, on first launch, where to save output); run from a
# terminal with arguments it forwards them straight to the real binary, e.g.
#   Polyphorm.app/Contents/MacOS/Polyphorm --dataset /path/to/mydata --agents 2000000
#
# The output folder (exports, imgui.ini, and on Finder launches
# polyphorm.log) defaults to ~/Polyphorm. The choice is stored with
# `defaults` under $DOMAIN OutputDir — ~/Library/Preferences, which needs
# no privacy permission — and handed to the binary as POLYPHORM_OUTPUT_DIR.
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/polyphorm-bin"
RES="$HERE/../Resources"
DOMAIN="io.github.rulkens.polyphorm"
DEFAULT_OUT="$HOME/Polyphorm"
AGENTS=1000000
EXTRA=""

# Old macOS versions pass a -psn_* process serial number on Finder launch.
case "$1" in -psn_*) shift ;; esac

OUT="$(defaults read "$DOMAIN" OutputDir 2>/dev/null)"

if [ $# -gt 0 ]; then
    # Terminal use: never block on a dialog; saved folder or the default.
    # An explicit POLYPHORM_OUTPUT_DIR in the environment wins.
    POLYPHORM_OUTPUT_DIR="${POLYPHORM_OUTPUT_DIR:-${OUT:-$DEFAULT_OUT}}" exec "$BIN" "$@"
fi

# All AppleScript gets its strings through argv, so paths with quotes or
# backslashes cannot break the script.
dialog() {
    osascript - "$1" >/dev/null 2>&1 <<'EOF'
on run argv
    display dialog (item 1 of argv) with title "Polyphorm" buttons {"OK"} default button 1 with icon caution
end run
EOF
}

# Ask where output goes; prints the chosen folder (default on any dismissal)
# and remembers it.
ask_output() {
    R=$(osascript - "$DEFAULT_OUT" 2>/dev/null <<'EOF'
on run argv
    set d to item 1 of argv
    set b to button returned of (display dialog "Where should Polyphorm save its exports and log?" & return & return & "Default: " & d with title "Polyphorm" buttons {"Choose Folder…", "Use Default"} default button "Use Default")
    if b is "Use Default" then return d
    try
        return POSIX path of (choose folder with prompt "Choose where Polyphorm saves its exports and log")
    on error
        return d
    end try
end run
EOF
)
    R="${R%/}"
    [ -n "$R" ] || R="$DEFAULT_OUT"
    defaults write "$DOMAIN" OutputDir -string "$R"
    printf '%s\n' "$R"
}

writable() { { mkdir -p "$1" && : >>"$1/polyphorm.log"; } 2>/dev/null; }

[ -n "$OUT" ] || OUT="$(ask_output)"
# A folder inside Documents/Desktop/Downloads can lose its privacy grant
# (the ad-hoc signed app changes identity with every release): ask again
# rather than quitting silently.
if ! writable "$OUT"; then
    dialog "Polyphorm cannot write to $OUT. Please choose another output folder (the default ~/Polyphorm always works)."
    OUT="$(ask_output)"
    writable "$OUT" || { dialog "Polyphorm cannot write to $OUT either. Giving up."; exit 1; }
fi

exec >>"$OUT/polyphorm.log" 2>&1
echo "=== $(date) ==="
echo "output: $OUT"

while :; do
    CHOICE=$(osascript - "$OUT" <<'EOF'
on run argv
    set opts to {"2MRS — 34,974 galaxies (all-sky)", "SDSS — 324,901 galaxies (full)", "SDSS — 37,655 galaxies (quick slice)", "Open my own dataset…", "Change output folder…"}
    set r to choose from list opts with title "Polyphorm" with prompt "Which dataset should Polyphorm load?" & return & "Output: " & (item 1 of argv) default items {item 1 of opts}
    if r is false then return ""
    return item 1 of r
end run
EOF
)
    case "$CHOICE" in
        Change*)
            NEW="$(ask_output)"
            if writable "$NEW"; then
                OUT="$NEW"
                exec >>"$OUT/polyphorm.log" 2>&1
                echo "=== $(date) ==="
                echo "output: $OUT"
            else
                dialog "Polyphorm cannot write to $NEW; keeping $OUT."
                defaults write "$DOMAIN" OutputDir -string "$OUT"
            fi
            continue ;;
        2MRS*)          DATASET="data/2MRS/2mrs_gui" ;;
        "SDSS — 324"*)  DATASET="data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0" ;;
        "SDSS — 37"*)   DATASET="data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0" ;;
        Open*)
            PICKED=$(osascript -e 'POSIX path of (choose file with prompt "Choose a CSV (x,y,z,weight rows) or a packed .bin / _metadata.txt dataset")' 2>/dev/null) || continue
            case "$PICKED" in
                *.csv|*.CSV)
                    DATASET="$PICKED"
                    # The simulation uses log10(1 + weight) / mean weight, so the
                    # weight units matter; ask, defaulting to "as is".
                    SCALE=$(osascript - 2>/dev/null <<'EOF'
on run argv
    set t to text returned of (display dialog "Weight scale for this CSV (multiplies the 4th column):" & return & return & "1 = use the weights as they are." & return & "0.001 = 1e9 → 1e12 Msun, as in the published SDSS VAC run (sample_3D_linW.csv)." with title "Polyphorm" default answer "1" buttons {"Cancel", "Load"} default button "Load")
    return t
end run
EOF
) || continue
                    case "$SCALE" in
                        ''|*[!0-9.eE+-]*) dialog "Weight scale must be a number, e.g. 1 or 0.001."; continue ;;
                    esac
                    EXTRA="--weight-scale $SCALE"
                    ;;
                *_metadata.txt) DATASET="${PICKED%_metadata.txt}" ;;
                *.bin)          DATASET="${PICKED%.bin}" ;;
                *) dialog "Please choose a .csv, or a .bin / _metadata.txt pair produced by pack_catalog.py."; continue ;;
            esac
            ;;
        *) exit 0 ;;
    esac
    break
done

case "$DATASET" in /*) CHECK="$DATASET" ;; *) CHECK="$RES/$DATASET" ;; esac
case "$CHECK" in
    *.csv|*.CSV) [ -f "$CHECK" ] || { dialog "Dataset not found: $CHECK"; exit 1; } ;;
    *)
        if [ ! -f "$CHECK.bin" ] || [ ! -f "${CHECK}_metadata.txt" ]; then
            dialog "Dataset not found: both $(basename "$CHECK").bin and $(basename "$CHECK")_metadata.txt are needed, in the same folder."
            exit 1
        fi ;;
esac

echo "dataset: $DATASET $EXTRA"
# $EXTRA is unquoted on purpose: empty, or "--weight-scale <number>".
# shellcheck disable=SC2086
POLYPHORM_OUTPUT_DIR="$OUT" "$BIN" --dataset "$DATASET" --agents "$AGENTS" $EXTRA
STATUS=$?
if [ $STATUS -ne 0 ]; then
    dialog "Polyphorm exited with an error (code $STATUS). Details are in $OUT/polyphorm.log"
fi
exit $STATUS
