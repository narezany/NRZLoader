#!/usr/bin/env python3
"""Standalone build report: no repository checkout, no dependencies.

Small enough to paste into a Termux session in one go. Reads the copy of the
game already installed on the device and prints the names the loader needs.

    python quick_report.py                  # find the installed game
    python quick_report.py a.apk b.apk      # or point it at files
"""

import os
import re
import struct
import subprocess
import sys
import zipfile

PACKAGE = "com.mojang.minecraftpe"
EVENTS = {
    "level.tick": r"Level4tickEv",
    "actor.hurt": r"Actor4hurtE",
    "player.join": r"9addPlayerE",
    "player.leave": r"12removePlayerE",
}


def run_pm(arguments):
    try:
        return subprocess.run(
            ["pm"] + arguments, capture_output=True, text=True, timeout=120
        ).stdout
    except Exception as error:
        print(f"  pm {' '.join(arguments)} failed: {error}")
        return ""


def paths_for(package):
    """Paths of every APK belonging to a package, empty entries dropped.

    `pm path` sometimes prints a bare `package:` line, which would otherwise
    turn into an empty filename further down.
    """
    output = run_pm(["path", package])
    paths = [line[8:].strip() for line in output.split() if line.startswith("package:")]
    return [path for path in paths if path]


def discover_packages():
    """Any installed package that looks like Minecraft."""
    output = run_pm(["list", "packages"])
    names = [line[8:].strip() for line in output.split() if line.startswith("package:")]
    pattern = re.compile(r"minecraft|mojang", re.IGNORECASE)
    return [name for name in names if name and pattern.search(name)]


def apk_paths():
    if len(sys.argv) > 1:
        return sys.argv[1:]

    package = os.environ.get("MCBE_PACKAGE", PACKAGE)
    paths = paths_for(package)
    if paths:
        return paths

    print(f"{package} gave nothing back; looking for other candidates")
    candidates = discover_packages()
    if not candidates:
        sys.exit(
            "No Minecraft-looking package is installed, or pm is unavailable here.\n"
            "Check with:  pm list packages | grep -i minecraft\n"
            "Then either: MCBE_PACKAGE=<name> python quick_report.py\n"
            "or pass the APK paths directly."
        )

    print(f"candidates: {', '.join(candidates)}")
    collected = []
    for candidate in candidates:
        found = paths_for(candidate)
        if found:
            print(f"  {candidate}: {len(found)} apk(s)")
            collected.extend(found)
        else:
            print(f"  {candidate}: no readable paths")

    if not collected:
        sys.exit("Found the package but not its files. Extract the APK manually instead.")
    return collected


def read_library(paths):
    for path in paths:
        try:
            archive = zipfile.ZipFile(path)
        except Exception as error:
            print(f"  skip {path}: {error}")
            continue
        names = [n for n in archive.namelist() if n.endswith("libminecraftpe.so")]
        preferred = [n for n in names if "arm64" in n] or names
        if preferred:
            print(f"reading {preferred[0]} from {path}")
            return archive.read(preferred[0])
        print(f"  {path}: no native library here")
    sys.exit("libminecraftpe.so not found in any of those files")


def read_symbols(image):
    if image[:4] != b"\x7fELF" or image[4] != 2:
        sys.exit("not a 64-bit ELF library")
    section_offset, = struct.unpack_from("<Q", image, 0x28)
    entry_size, count = struct.unpack_from("<HH", image, 0x3A)
    if section_offset == 0 or count == 0:
        sys.exit("this build has no section headers, so no symbol names")

    sections = [
        struct.unpack_from("<IIQQQQIIQQ", image, section_offset + index * entry_size)
        for index in range(count)
    ]

    symbols = {}
    for section in sections:
        _name, kind, _flags, _addr, offset, size, link, _info, _align, entsize = section
        if kind not in (2, 11) or entsize == 0:
            continue
        strings_section = sections[link]
        strings = image[strings_section[4] : strings_section[4] + strings_section[5]]
        for index in range(size // entsize):
            name_offset, _info2, _other, _shndx, value, _size = struct.unpack_from(
                "<IBBHQQ", image, offset + index * entsize
            )
            if value == 0 or name_offset == 0:
                continue
            end = strings.find(b"\0", name_offset)
            symbols.setdefault(strings[name_offset:end].decode("utf-8", "replace"), value)
    return symbols


def main():
    image = read_library(apk_paths())
    symbols = read_symbols(image)

    print(f"\n# symbols: {len(symbols)}")

    print("\n## event candidates")
    for event, pattern in EVENTS.items():
        expression = re.compile(pattern)
        matches = sorted(name for name in symbols if expression.search(name))
        print(f"  {event}:")
        for name in matches[:6] or ["(nothing matched)"]:
            print(f"    0x{symbols[name]:08x}  {name}" if name in symbols else f"    {name}")

    print("\n## biggest classes")
    class_pattern = re.compile(r"^_ZN(\d+)(\D)")
    counts = {}
    for name in symbols:
        match = class_pattern.match(name)
        if not match:
            continue
        length = int(match.group(1))
        class_name = name[3 + len(match.group(1)) :][:length]
        counts[class_name] = counts.get(class_name, 0) + 1
    for class_name, total in sorted(counts.items(), key=lambda item: -item[1])[:30]:
        print(f"  {total:6d}  {class_name}")

    print("\n## a few Level and Player members")
    interesting = sorted(n for n in symbols if re.search(r"^_ZN(5Level|6Player|5Actor)", n))
    for name in interesting[:25]:
        print(f"  0x{symbols[name]:08x}  {name}")


if __name__ == "__main__":
    main()
