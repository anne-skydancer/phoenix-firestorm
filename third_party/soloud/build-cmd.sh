#!/usr/bin/env bash
set -eu

TOP="$(cd "$(dirname "$0")" && pwd)"
UPSTREAM="https://github.com/jarikomppa/soloud.git"
REF="e82fd32c1f62183922f08c14c814a02b58db1873"
VERSION="20240813.e82fd32-fsvs1"
SRC="$TOP/src"
BUILD="$TOP/build"
STAGE="$TOP/stage"
GIT_EXE="${GIT:-git}"
CMAKE_EXE="${CMAKE:-cmake}"

if [ -n "${AUTOBUILD_VARIABLES_FILE:-}" ]; then
    # shellcheck disable=SC1090
    source "$AUTOBUILD_VARIABLES_FILE"
fi

for generated in "$SRC" "$BUILD" "$STAGE"; do
    if [ -e "$generated" ]; then
        echo "Generated path already exists; clean it before rebuilding: $generated" >&2
        exit 1
    fi
done
"$GIT_EXE" clone --filter=blob:none --no-checkout "$UPSTREAM" "$SRC"
"$GIT_EXE" -C "$SRC" fetch --depth 1 origin "$REF"
"$GIT_EXE" -C "$SRC" checkout --detach "$REF"

for patch in "$TOP"/patches/[0-9][0-9][0-9][0-9]-*.patch; do
    "$GIT_EXE" -C "$SRC" apply --check "$patch"
    "$GIT_EXE" -C "$SRC" apply "$patch"
done

platform="${AUTOBUILD_PLATFORM:-windows64}"
case "$platform" in
    windows|windows64)
        generator="${AUTOBUILD_WIN_CMAKE_GEN:-Visual Studio 17 2022}"
        "$CMAKE_EXE" -S "$TOP" -B "$BUILD" -G "$generator" -A x64 \
            -DSOLOUD_SOURCE_DIR="$SRC"
        "$CMAKE_EXE" --build "$BUILD" --config Release --parallel
        "$CMAKE_EXE" --install "$BUILD" --config Release --prefix "$STAGE/release"
        "$CMAKE_EXE" --build "$BUILD" --config Debug --parallel
        "$CMAKE_EXE" --install "$BUILD" --config Debug --prefix "$STAGE/debug"

        mkdir -p "$STAGE/lib/release" "$STAGE/lib/debug" \
                 "$STAGE/include/soloud" "$STAGE/LICENSES"
        cp "$STAGE/release/lib/soloud.lib" "$STAGE/lib/release/"
        cp "$STAGE/debug/lib/soloud.lib" "$STAGE/lib/debug/"
        cp -R "$STAGE/release/include/soloud/." "$STAGE/include/soloud/"
        cp "$STAGE/release/LICENSES/soloud.txt" "$STAGE/LICENSES/"
        rm -rf "$STAGE/release" "$STAGE/debug"
        ;;
    linux|linux64)
        "$CMAKE_EXE" -S "$TOP" -B "$BUILD" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release -DSOLOUD_SOURCE_DIR="$SRC"
        "$CMAKE_EXE" --build "$BUILD" --parallel
        "$CMAKE_EXE" --install "$BUILD" --prefix "$STAGE/install"

        mkdir -p "$STAGE/lib/release" "$STAGE/include/soloud" "$STAGE/LICENSES"
        cp "$STAGE/install/lib/libsoloud.a" "$STAGE/lib/release/"
        cp -R "$STAGE/install/include/soloud/." "$STAGE/include/soloud/"
        cp "$STAGE/install/LICENSES/soloud.txt" "$STAGE/LICENSES/"
        rm -rf "$STAGE/install"
        ;;
    *)
        echo "Unsupported Autobuild platform: $platform" >&2
        exit 1
        ;;
esac

printf '%s\n' "$VERSION" > "$STAGE/VERSION.txt"
printf '%s\n' "$REF" > "$STAGE/SOURCE_COMMIT.txt"
