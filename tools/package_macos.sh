#!/bin/sh
# Build a distributable Polyphorm.app (Apple Silicon, macOS 13+) and zip it.
#
#   tools/package_macos.sh v1.2.0-alpha
#
# Output: dist/Polyphorm-<version>-macos-arm64.zip (v1.2.0-alpha ->
# 1.2.0-alpha) containing Polyphorm.app, README.txt, pack_catalog.py and
# sdss_vac_1e12Msun.csv (the example CSV).
# Info.plist gets the numeric part only (1.2.0), as macOS expects. Uses a separate build-release/ tree
# (deployment target 13.0) that reuses build/'s Dawn and ImGui checkouts,
# so the dev build/ is untouched. The app is ad-hoc signed only; see
# packaging/macos/README.txt for the first-launch Gatekeeper steps.
set -eu

TAG="${1:?usage: tools/package_macos.sh <tag, e.g. v1.2.0-alpha>}"
VERSION="$(echo "$TAG" | sed -E 's/^v//; s/-macos-port$//')"
PLIST_VERSION="$(echo "$VERSION" | sed -E 's/^([0-9]+(\.[0-9]+)*).*/\1/')"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-release"
DIST="$ROOT/dist"
STAGE="$DIST/Polyphorm-$VERSION-macos-arm64"
APP="$STAGE/Polyphorm.app"
ZIP="$STAGE.zip"

# Datasets shipped in the bundle, relative to bin/ (base names, no suffix).
DATASETS="
data/2MRS/2mrs_gui
data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0
data/SDSS/galaxiesInSdssSlice_viz_bigger_lumdist_t=0.0
"

cd "$ROOT"

# Fail early with the fix, not halfway through an hour-long build.
command -v cmake >/dev/null || { echo "cmake not found: brew install cmake" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja not found: brew install ninja" >&2; exit 1; }
SDSS_FULL="bin/data/SDSS/sdssGalaxy_rsdCorr_dbscan_e2p0ms3_dz0p001_m10p0_t=0.0"
if [ ! -f "$SDSS_FULL.bin" ] && ! "$ROOT/.venv/bin/python" -c "import numpy" 2>/dev/null; then
    echo "$SDSS_FULL.bin is not in git and must be regenerated, which needs the" >&2
    echo "repo venv: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt" >&2
    exit 1
fi

echo "==> configure + build ($BUILD)"
DEPS=""
[ -d build/_deps/dawn-src ] && DEPS="$DEPS -DFETCHCONTENT_SOURCE_DIR_DAWN=$ROOT/build/_deps/dawn-src"
[ -d build/_deps/imgui-src ] && DEPS="$DEPS -DFETCHCONTENT_SOURCE_DIR_IMGUI=$ROOT/build/_deps/imgui-src"
# shellcheck disable=SC2086
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 $DEPS >/dev/null
cmake --build "$BUILD" --target polyphorm -j "$(sysctl -n hw.ncpu)"

echo "==> datasets"
if [ ! -f "$SDSS_FULL.bin" ]; then
    echo "   $SDSS_FULL.bin is not tracked in git; regenerating from the vendored VAC CSV"
    "$ROOT/.venv/bin/python" tools/pack_vac_catalog.py
fi
for d in $DATASETS; do
    [ -f "bin/$d.bin" ] && [ -f "bin/${d}_metadata.txt" ] || { echo "missing bin/$d.{bin,_metadata.txt}" >&2; exit 1; }
done

echo "==> assemble $APP"
rm -rf "$STAGE" "$ZIP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/shaders" "$APP/Contents/Resources/data"
sed "s/@VERSION@/$PLIST_VERSION/g" packaging/macos/Info.plist > "$APP/Contents/Info.plist"
install -m 755 packaging/macos/launcher.sh "$APP/Contents/MacOS/Polyphorm"
install -m 755 "$BUILD/polyphorm" "$APP/Contents/MacOS/polyphorm-bin"
cp shaders/*.wgsl "$APP/Contents/Resources/shaders/"
cp bin/config.polyp packaging/macos/Polyphorm.icns "$APP/Contents/Resources/"
cp bin/data/palette_*.tga "$APP/Contents/Resources/data/"
for d in $DATASETS; do
    mkdir -p "$APP/Contents/Resources/$(dirname "$d")"
    cp "bin/$d.bin" "bin/${d}_metadata.txt" "$APP/Contents/Resources/$(dirname "$d")/"
done
# Underline the title to its substituted length.
sed "s/@VERSION@/$VERSION/g; s/@TAG@/$TAG/g" packaging/macos/README.txt \
    | awk 'NR == 1 { t = length($0) } NR == 2 { s = ""; for (i = 0; i < t; i++) s = s "="; print s; next } { print }' \
    > "$STAGE/README.txt"
cp tools/pack_catalog.py bin/data/SDSS/sdss_vac_1e12Msun.csv "$STAGE/"

echo "==> ad-hoc sign"
codesign --force --sign - "$APP/Contents/MacOS/polyphorm-bin"
codesign --force --sign - "$APP"
codesign --verify --strict --verbose=1 "$APP"

echo "==> zip"
( cd "$DIST" && ditto -c -k --keepParent "$(basename "$STAGE")" "$(basename "$ZIP")" )
ls -lh "$ZIP"
shasum -a 256 "$ZIP"
