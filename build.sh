#!/usr/bin/env bash
# Builds the loader and the example mod for arm64-v8a.
#
#   ANDROID_NDK=/path/to/ndk ./build.sh
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ndk="${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}"
build_dir="${BUILD_DIR:-$root/build}"

if [[ -z "$ndk" || ! -f "$ndk/build/cmake/android.toolchain.cmake" ]]; then
    echo "Set ANDROID_NDK to an NDK r25 or newer, for example:"
    echo "  ANDROID_NDK=\$HOME/Android/Sdk/ndk/27.2.12479018 ./build.sh"
    exit 1
fi

cmake -B "$build_dir" -S "$root" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$build_dir" --parallel

echo
echo "loader:  $build_dir/libnrzloader.so"
echo "example: $build_dir/examples/hello_mod/libhello_mod.so"
