#!/usr/bin/env bash
# Builds and runs the test suite for AArch64 under an emulator, so the hook
# engine is exercised on the architecture it actually ships to.
#
# Needs: g++-aarch64-linux-gnu, qemu-user, python3
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${TMPDIR:-/tmp}/mcbe-tests"
cxx="${CROSS_CXX:-aarch64-linux-gnu-g++}"
qemu="${QEMU:-qemu-aarch64}"
sysroot="${SYSROOT:-/usr/aarch64-linux-gnu}"

for tool in "$cxx" "$qemu" python3; do
    command -v "$tool" >/dev/null || { echo "missing: $tool"; exit 1; }
done

rm -rf "$work"
mkdir -p "$work"

echo "== building =="
"$cxx" -O1 -g -static -o "$work/test_arm64" \
    "$root/tests/test_arm64.cpp" "$root/tests/prologues.S" \
    "$root/src/hook/arm64_reloc.cpp" "$root/src/hook/inline_hook.cpp" \
    -I"$root/src" -I"$root/include"

"$cxx" -O1 -g -shared -fPIC -o "$work/libfakegame.so" "$root/tests/fake_game.cpp"

"$cxx" -O1 -g -o "$work/test_symbols" \
    "$root/tests/test_symbols.cpp" "$root/src/core/elf_image.cpp" \
    "$root/src/core/module.cpp" "$root/src/core/symbols.cpp" "$root/src/core/log.cpp" \
    -I"$root/src" -I"$root/include" -ldl

python3 - "$work" <<'PY'
import sys, zipfile
work = sys.argv[1]
with zipfile.ZipFile(f"{work}/archive.zip", "w") as archive:
    archive.write(f"{work}/libfakegame.so", "lib/arm64-v8a/libfakegame.so",
                  compress_type=zipfile.ZIP_STORED)
    archive.writestr("AndroidManifest.xml", "placeholder")
PY

echo
echo "== hook engine =="
"$qemu" "$work/test_arm64"

echo
echo "== symbol resolution =="
"$qemu" -L "$sysroot" -E LD_LIBRARY_PATH="$work" \
    "$work/test_symbols" "$work/libfakegame.so" "$work/archive.zip"

"$cxx" -O1 -g -o "$work/test_paths" \
    "$root/tests/test_paths.cpp" "$root/src/core/paths.cpp" -I"$root/src" -I"$root/include"

echo
echo "== directory layout =="
"$qemu" -L "$sysroot" "$work/test_paths" "$work/paths"

zlib_prefix="${ZLIB_AARCH64:-/opt/zlib-aarch64}"
if [ -f "$zlib_prefix/lib/libz.a" ]; then
    "$cxx" -O1 -g -o "$work/test_apk_assets" \
        "$root/tests/test_apk_assets.cpp" "$root/src/core/apk_assets.cpp" \
        "$root/src/core/log.cpp" -I"$root/src" -I"$root/include" \
        -I"$zlib_prefix/include" -L"$zlib_prefix/lib" -lz

    python3 - "$work" <<'PY'
import sys, zipfile
work = sys.argv[1]
body = '{"namespace":"start",' + ' ' * 100 + '"play_button":{"type":"button"},'
body += 'x' * (20000 - len(body) - 1) + '}'
with zipfile.ZipFile(f"{work}/ui.apk", "w") as archive:
    archive.writestr("assets/ui/start_screen.json", body, compress_type=zipfile.ZIP_DEFLATED)
    archive.writestr("assets/ui/stored.json", '{"stored":true}', compress_type=zipfile.ZIP_STORED)
    archive.write(f"{work}/libfakegame.so", "lib/arm64-v8a/libminecraftpe.so",
                  compress_type=zipfile.ZIP_STORED)
PY

    echo
    echo "== game package reading =="
    "$qemu" -L "$sysroot" "$work/test_apk_assets" "$work/ui.apk"
else
    echo
    echo "== game package reading =="
    echo "  skipped: no aarch64 zlib at $zlib_prefix (set ZLIB_AARCH64)"
fi

echo
echo "== android manifest editing =="
python3 "$root/tests/test_axml.py"

echo
echo "== injection mechanism =="
cat > "$work/probe.cpp" <<'EOF'
#include <cstdio>
__attribute__((constructor)) static void probe() { printf("loader constructor ran\n"); }
EOF
cat > "$work/host.cpp" <<'EOF'
#include <dlfcn.h>
#include <cstdio>
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    void* handle = dlopen(argv[1], RTLD_NOW);
    if (handle == nullptr) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto call = reinterpret_cast<int (*)(int)>(dlsym(handle, "fake_game_call_internal"));
    printf("game library usable: %s\n", call && call(1) == 78 ? "yes" : "no");
    return call && call(1) == 78 ? 0 : 1;
}
EOF
"$cxx" -shared -fPIC -o "$work/libprobe.so" "$work/probe.cpp"
"$cxx" -o "$work/host" "$work/host.cpp" -ldl
cp "$work/libfakegame.so" "$work/libfakegame_patched.so"
if command -v patchelf >/dev/null; then
    patchelf --add-needed libprobe.so "$work/libfakegame_patched.so"
    "$qemu" -L "$sysroot" -E LD_LIBRARY_PATH="$work" "$work/host" "$work/libfakegame_patched.so"
else
    echo "skipped: patchelf not installed"
fi

echo
echo "all suites passed"
