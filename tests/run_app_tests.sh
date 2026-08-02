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
echo "== reading class names out of the game =="
# Pure JVM, no device: the verdict this produces decides whether reaching the
# game's own code is worth attempting at all.
( cd "$root" && gradle --console=plain -q :app:testReleaseUnitTest ) && \
    python3 - "$root" <<'REPORT'
import glob, re, sys
for path in glob.glob(sys.argv[1] + "/app/build/test-results/testReleaseUnitTest/*.xml"):
    text = open(path).read()
    counts = re.search(r'tests="(\d+)".*?failures="(\d+)".*?errors="(\d+)"', text)
    for name in re.findall(r'testcase name="([^"]+)"', text):
        print("  ok    " + name)
    print("  %s checks, %s failures, %s errors" % counts.groups())
    if counts.group(2) != "0" or counts.group(3) != "0":
        sys.exit(1)
REPORT

echo
echo "== the loader version says the same thing everywhere =="
# A mod declares which loader versions it works with, so this number carries
# meaning; three copies of it that can drift apart would make that meaningless.
declared="$(tr -d ' \t\r\n' < "$root/VERSION")"
in_header="$(sed -n 's/.*#define MCBE_LOADER_VERSION "\(.*\)".*/\1/p' \
    "$root/include/mcbe/mod_api.h")"
in_launcher="$(sed -n 's/.*const val VALUE = "\(.*\)".*/\1/p' \
    "$root/app/src/main/java/ru/narezany/nrzloader/core/LoaderVersion.kt")"

echo "  VERSION        $declared"
echo "  mod_api.h      $in_header"
echo "  LoaderVersion  $in_launcher"
if [ "$declared" != "$in_header" ] || [ "$declared" != "$in_launcher" ]; then
    echo "  FAIL  they disagree"
    exit 1
fi
echo "  ok    all three agree"

echo
echo "all app suites passed"
