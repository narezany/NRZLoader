#!/usr/bin/env bash
# Builds the classes that get added to the game package.
#
#   ANDROID_SDK=/path/to/sdk ./bootstrap/build.sh
#
# The result is app/src/main/assets/nrz_bootstrap.dex, which the patcher writes
# into the rebuilt package as an extra classes*.dex.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"

sdk="${ANDROID_SDK:-${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}}"
if [[ -z "$sdk" && -f "$root/local.properties" ]]; then
    sdk="$(sed -n 's/^sdk\.dir=//p' "$root/local.properties" | head -1)"
fi

platform="$(ls -d "$sdk"/platforms/android-* 2>/dev/null | sort -V | tail -1 || true)"
tools="$(ls -d "$sdk"/build-tools/* 2>/dev/null | sort -V | tail -1 || true)"

if [[ ! -f "$platform/android.jar" || ! -x "$tools/d8" ]]; then
    echo "Set ANDROID_SDK to an SDK with platform and build-tools installed."
    exit 1
fi

out="$here/out"
rm -rf "$out"
mkdir -p "$out"

javac -source 8 -target 8 -nowarn \
    -bootclasspath "$platform/android.jar" \
    -d "$out" \
    "$here"/src/ru/narezany/nrzloader/*.java

"$tools/d8" --min-api 26 --output "$out" $(find "$out" -name '*.class')
cp "$out/classes.dex" "$root/app/src/main/assets/nrz_bootstrap.dex"

echo "bootstrap dex: $root/app/src/main/assets/nrz_bootstrap.dex"
