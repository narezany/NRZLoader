#!/data/data/com.termux/files/usr/bin/bash
# Reads the symbol names out of the Minecraft you already have installed.
#
# Runs entirely on the phone in Termux. Nothing is downloaded and nothing
# leaves the device: it reads your own installed copy and writes text files.
#
#   pkg install python
#   bash on_device.sh
set -euo pipefail

package="${PACKAGE:-com.mojang.minecraftpe}"
output="${OUTPUT:-$HOME/mcbeloader-out}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v python3 >/dev/null || { echo "python is missing: pkg install python"; exit 1; }

echo "looking for $package"

# `pm path` lists every APK of the install: the base plus, on modern versions,
# one split per ABI. The native library is usually in the arm64 split.
if ! paths="$(pm path "$package" 2>/dev/null)"; then
    echo "pm is unavailable. Is this Termux on the same device as the game?"
    exit 1
fi
if [ -z "$paths" ]; then
    echo "$package is not installed."
    echo "If your Minecraft has a different package name, run:"
    echo "  PACKAGE=<name> bash on_device.sh"
    exit 1
fi

apks=()
while read -r line; do
    [ -z "$line" ] && continue
    apk="${line#package:}"
    if [ -r "$apk" ]; then
        apks+=("$apk")
        echo "  found $apk"
    else
        echo "  cannot read $apk (skipping)"
    fi
done <<< "$paths"

if [ "${#apks[@]}" -eq 0 ]; then
    echo
    echo "None of the APKs are readable from Termux."
    echo "Extract the APK with an app like MT Manager, then run:"
    echo "  python3 symgen.py /sdcard/Download/minecraft.apk -o $output"
    exit 1
fi

mkdir -p "$output"
echo
python3 "$here/symgen.py" "${apks[@]}" -o "$output"

echo
echo "Done. The small file worth sharing is:"
echo "  $output/report.txt"
echo
echo "Open it with:  cat $output/report.txt"
