#!/usr/bin/env python3
"""Discovers Minecraft's function names inside your own copy of the game.

The loader needs to know where functions live, and those addresses change with
every build. Rather than shipping guesses, this reads the symbol tables out of
libminecraftpe.so and writes two files the loader consumes at runtime:

    symbols.map    every symbol and its offset, for builds where the runtime
                   symbol tables are unavailable
    bindings.conf  a starting point mapping loader events to real symbol names

Usage:
    python3 symgen.py <minecraft.apk | libminecraftpe.so> [-o output_dir]
    python3 symgen.py <file> --search Level::tick
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass

ELF_MAGIC = b"\x7fELF"

# Loader event -> patterns that usually identify the right function. The first
# pattern that matches a single symbol wins; ambiguous matches are reported so
# you can pick by hand.
EVENT_PATTERNS = {
    "level.tick": [r"^_ZN5Level4tickEv$", r"^_ZN\d+Level4tickEv$"],
    "actor.hurt": [r"^_ZN5Actor4hurtERK16ActorDamageSourcefbb$", r"^_ZN5Actor4hurtERK\w+f"],
    "player.join": [r"^_ZN5Level9addPlayerE", r"addPlayer"],
    "player.leave": [r"^_ZN5Level12removePlayerE", r"removePlayer"],
}


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    is_function: bool


def read_elf_bytes(paths: list[str]) -> tuple[bytes, str]:
    """Returns the ELF image, pulling it out of an APK when needed.

    Recent Minecraft installs are split into several APKs and the native
    library lives in the ABI specific one, not in the base, so several paths
    may be handed in and the first that carries the library wins.
    """
    searched = []

    for path in paths:
        if not os.path.isfile(path):
            searched.append(f"{path}: no such file")
            continue

        with open(path, "rb") as handle:
            head = handle.read(4)

        if head == ELF_MAGIC:
            with open(path, "rb") as handle:
                return handle.read(), path

        if not zipfile.is_zipfile(path):
            searched.append(f"{path}: not an ELF file or an APK")
            continue

        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            candidates = [
                name for name in names if name.endswith("libminecraftpe.so") and "arm64-v8a" in name
            ]
            if not candidates:
                candidates = [name for name in names if name.endswith("libminecraftpe.so")]
            if not candidates:
                searched.append(f"{os.path.basename(path)}: no libminecraftpe.so")
                continue

            chosen = candidates[0]
            print(f"reading {chosen} from {os.path.basename(path)}")
            return archive.read(chosen), chosen

    report = "\n  ".join(searched)
    sys.exit(
        "no libminecraftpe.so found. Checked:\n  " + report + "\n\n"
        "A split install keeps the library in the ABI specific APK; pass every\n"
        "path that `pm path com.mojang.minecraftpe` printed."
    )


def parse_symbols(image: bytes) -> list[Symbol]:
    if image[:4] != ELF_MAGIC:
        sys.exit("not an ELF image")
    if image[4] != 2:
        sys.exit("only 64-bit libraries are supported (arm64-v8a)")

    e_shoff, = struct.unpack_from("<Q", image, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", image, 0x3A)
    if e_shoff == 0 or e_shnum == 0:
        sys.exit("this library has no section headers; nothing to read")

    sections = []
    for index in range(e_shnum):
        base = e_shoff + index * e_shentsize
        name, sh_type, _flags, _addr, offset, size, link, _info, _align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", image, base
        )
        sections.append((name, sh_type, offset, size, link, entsize))

    SHT_SYMTAB, SHT_DYNSYM = 2, 11
    symbols: dict[str, Symbol] = {}

    for _name, sh_type, offset, size, link, entsize in sections:
        if sh_type not in (SHT_SYMTAB, SHT_DYNSYM) or entsize == 0:
            continue
        _n, _t, str_offset, str_size, _l, _e = sections[link]
        strings = image[str_offset : str_offset + str_size]

        for index in range(size // entsize):
            base = offset + index * entsize
            st_name, st_info, _other, _shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", image, base
            )
            if st_value == 0 or st_name == 0:
                continue
            end = strings.find(b"\0", st_name)
            name = strings[st_name:end].decode("utf-8", "replace")
            if not name:
                continue
            symbols.setdefault(
                name, Symbol(name, st_value, st_size, (st_info & 0xF) == 2)
            )

    return list(symbols.values())


def demangle(names: list[str]) -> dict[str, str]:
    """Best effort: uses c++filt when available."""
    if not shutil.which("c++filt") or not names:
        return {}
    try:
        result = subprocess.run(
            ["c++filt"], input="\n".join(names), capture_output=True, text=True, timeout=120
        )
        readable = result.stdout.splitlines()
        return dict(zip(names, readable)) if len(readable) == len(names) else {}
    except (subprocess.SubprocessError, OSError):
        return {}


def write_symbol_map(symbols: list[Symbol], path: str) -> None:
    with open(path, "w", encoding="utf-8") as out:
        out.write("# name<TAB>offset, generated by symgen.py\n")
        for symbol in sorted(symbols, key=lambda s: s.value):
            out.write(f"{symbol.name}\t0x{symbol.value:x}\n")
    print(f"wrote {path} ({len(symbols)} symbols)")


def suggest_bindings(symbols: list[Symbol], path: str) -> None:
    by_name = {symbol.name: symbol for symbol in symbols}
    lines = [
        "# Generated by symgen.py. Each line maps a loader event to the symbol",
        "# to hook. Verify these before trusting them: a wrong signature will",
        "# crash the game rather than fail quietly.",
        "#",
        "# A value may also be a byte signature, written as: sig:1F 20 03 D5 ?? ??",
        "",
    ]

    for event, patterns in EVENT_PATTERNS.items():
        matches: list[str] = []
        for pattern in patterns:
            expression = re.compile(pattern)
            matches = [name for name in by_name if expression.search(name)]
            if matches:
                break

        if not matches:
            lines.append(f"# {event}: no candidate found")
        elif len(matches) == 1:
            lines.append(f"{event} = {matches[0]}")
        else:
            lines.append(f"# {event}: {len(matches)} candidates, pick one")
            for name in sorted(matches)[:8]:
                lines.append(f"#   {name}")
            lines.append(f"# {event} = {sorted(matches)[0]}")
        lines.append("")

    with open(path, "w", encoding="utf-8") as out:
        out.write("\n".join(lines))
    print(f"wrote {path}")


def write_report(symbols: list[Symbol], source: str, path: str) -> None:
    """A short, pasteable summary of what this build looks like.

    Small enough to send to someone who does not have the game, and it carries
    no game code: only names and offsets.
    """
    by_name = {symbol.name: symbol for symbol in symbols}
    functions = [symbol for symbol in symbols if symbol.is_function]

    lines = [
        "# mcbe-modloader build report",
        f"# source: {source}",
        f"# symbols: {len(symbols)}, functions: {len(functions)}",
        "",
        "## version strings",
    ]

    version_like = re.compile(r"^\d+\.\d+\.\d+")
    versions = sorted({name for name in by_name if version_like.match(name)})
    lines.extend(f"  {name}" for name in versions[:10] or ["  (none found in symbol names)"])

    lines.append("")
    lines.append("## event candidates")
    for event, patterns in EVENT_PATTERNS.items():
        matches: list[str] = []
        for pattern in patterns:
            expression = re.compile(pattern)
            matches = [name for name in by_name if expression.search(name)]
            if matches:
                break
        lines.append(f"  {event}:")
        if not matches:
            lines.append("    (nothing matched)")
        for name in sorted(matches)[:6]:
            lines.append(f"    0x{by_name[name].value:08x}  {name}")

    lines.append("")
    lines.append("## sample of the biggest classes")
    class_pattern = re.compile(r"^_ZN(\d+)(\w+?)\d")
    counts: dict[str, int] = {}
    for name in by_name:
        match = class_pattern.match(name)
        if match:
            length = int(match.group(1))
            class_name = name[len(match.group(1)) + 3 :][:length]
            counts[class_name] = counts.get(class_name, 0) + 1
    for class_name, count in sorted(counts.items(), key=lambda item: -item[1])[:25]:
        lines.append(f"  {count:5d}  {class_name}")

    with open(path, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    print(f"wrote {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", nargs="+", help="minecraft APK(s) or libminecraftpe.so")
    parser.add_argument("-o", "--output", default="out", help="output directory")
    parser.add_argument("--search", help="print symbols containing this text and exit")
    parser.add_argument("--limit", type=int, default=40, help="max search results to print")
    arguments = parser.parse_args()

    image, source = read_elf_bytes(arguments.target)
    symbols = parse_symbols(image)
    if not symbols:
        print("no symbols found: this build is stripped.")
        print("You will need byte signatures instead; see README.md.")
        return 1

    functions = [symbol for symbol in symbols if symbol.is_function]
    print(f"{len(symbols)} symbols, {len(functions)} functions, from {source}")

    if arguments.search:
        needle = arguments.search
        matches = [s for s in symbols if needle in s.name]
        readable = demangle([s.name for s in matches[: arguments.limit]])
        if not matches:
            # A readable name was probably meant; search the demangled forms.
            everything = demangle([s.name for s in symbols])
            matches = [s for s in symbols if needle in everything.get(s.name, "")]
            readable = {name: everything[name] for name in (s.name for s in matches) if name in everything}

        print(f"{len(matches)} matches")
        for symbol in matches[: arguments.limit]:
            pretty = readable.get(symbol.name, symbol.name)
            print(f"  0x{symbol.value:08x}  {symbol.name}")
            if pretty != symbol.name:
                print(f"              {pretty}")
        return 0

    os.makedirs(arguments.output, exist_ok=True)
    write_symbol_map(symbols, os.path.join(arguments.output, "symbols.map"))
    suggest_bindings(symbols, os.path.join(arguments.output, "bindings.conf"))
    write_report(symbols, source, os.path.join(arguments.output, "report.txt"))

    print()
    print("Copy symbols.map and bindings.conf to the device at:")
    print("  /sdcard/Android/data/com.mojang.minecraftpe/files/mcbeloader/")
    print()
    print("report.txt is the small one, safe to share when asking for help.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
