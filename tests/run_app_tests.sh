#!/usr/bin/env bash
# Builds and runs the patch engine tests on a desktop JVM.
#
# Needs: a JDK, and optionally Android build-tools for apksigner verification.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${TMPDIR:-/tmp}/nrz-app-tests"
apksig="$root/app/libs/apksig.jar"

command -v javac >/dev/null || { echo "missing: javac"; exit 1; }
[ -f "$apksig" ] || { echo "missing: $apksig"; exit 1; }

rm -rf "$work"
mkdir -p "$work/classes"

echo "== building =="
javac -nowarn -cp "$apksig" -d "$work/classes" \
    "$root"/app/src/main/java/ru/narezany/nrzloader/patch/*.java \
    "$root"/app/src/test/java/ru/narezany/nrzloader/patch/*.java

echo
echo "== manifest editing =="
java -cp "$work/classes" ru.narezany.nrzloader.patch.AxmlTest \
    "$root/tests/fixtures/AndroidManifest.xml" "$work"

echo
echo "== building a stand-in package =="
python3 - "$root" "$work" <<'PY'
import sys, zipfile
root, work = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(f"{work}/game.apk", "w") as archive:
    archive.write(f"{root}/tests/fixtures/AndroidManifest.xml", "AndroidManifest.xml")
    archive.writestr("lib/arm64-v8a/libminecraftpe.so", b"\x7fELF" + b"\x00" * 4096,
                     zipfile.ZIP_STORED)
    archive.writestr("classes.dex", "dex" * 500)
    archive.writestr("assets/keep.txt", "keep me")
    archive.writestr("resources.arsc", "arsc" * 200)
    archive.writestr("META-INF/CERT.RSA", "old signature")
    archive.writestr("META-INF/MANIFEST.MF", "old manifest")
PY

echo
echo "== patching and signing =="
java -cp "$work/classes:$apksig" ru.narezany.nrzloader.patch.ApkPatcherTest \
    "$work/game.apk" "$work/patched.apk" \
    "$root/dist/libnrzloader.so" \
    "$root/app/src/main/assets/nrz_bootstrap.dex" \
    "$root/app/src/main/assets/nrzloader.p12"

echo
echo "== signature, checked by apksigner =="
signer=""
for candidate in "${ANDROID_HOME:-}/build-tools"/*/apksigner /opt/android-sdk/build-tools/*/apksigner; do
    [ -x "$candidate" ] && signer="$candidate" && break
done
if [ -n "$signer" ]; then
    # Checked against Android 8, the oldest the game itself supports; v1 is
    # deliberately not produced.
    "$signer" verify --verbose --min-sdk-version 26 "$work/patched.apk" | head -4
else
    echo "  skipped: apksigner not found"
fi

echo
echo "all app suites passed"
