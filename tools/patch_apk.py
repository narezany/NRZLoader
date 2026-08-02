#!/usr/bin/env python3
"""Injects the loader into a Minecraft APK you already own.

Rather than editing the app's compiled Java, this adds the loader as an ELF
dependency of libminecraftpe.so. The system linker then loads it automatically,
before the game runs any of its own code.

    python3 patch_apk.py minecraft.apk libmcbeloader.so -o patched.apk

The output still has to be signed before Android will install it. The script
signs it for you when apksigner is on PATH; otherwise it prints what to do.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import axml

GAME_LIBRARY = "libminecraftpe.so"
STORED_EXTENSIONS = (".so", ".arsc", ".png", ".ogg", ".mp3", ".jpg")


def find_apksigner() -> str | None:
    direct = shutil.which("apksigner")
    if direct:
        return direct

    home = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if not home:
        return None
    build_tools = os.path.join(home, "build-tools")
    if not os.path.isdir(build_tools):
        return None
    for version in sorted(os.listdir(build_tools), reverse=True):
        candidate = os.path.join(build_tools, version, "apksigner")
        if os.path.isfile(candidate):
            return candidate
    return None


ALL_FILES_PERMISSION = "android.permission.MANAGE_EXTERNAL_STORAGE"


def rename_application(manifest: bytes, label: str) -> bytes:
    try:
        renamed = axml.set_application_label(manifest, label)
        print(f"manifest: app renamed to {label}")
        return renamed
    except axml.AxmlError as error:
        print(f"manifest: could not rename the app ({error})")
        return manifest


def grant_storage_access(manifest: bytes) -> bytes:
    """Adds all-files access to the manifest, leaving it alone if already there.

    Without it the loader cannot create /sdcard/MCPELoader and falls back to a
    folder inside Android/data that file managers struggle to reach.
    """
    if axml.has_permission(manifest, ALL_FILES_PERMISSION):
        print("manifest already declares all-files access")
        return manifest

    patched = axml.add_permission(manifest, ALL_FILES_PERMISSION)
    print("manifest: added all-files access")
    return patched


def add_entry(archive: zipfile.ZipFile, name: str, data: bytes, alignment: int) -> None:
    """Writes an entry, padding so its payload starts on an aligned boundary.

    Android maps native libraries straight out of the APK, which only works
    when they are stored uncompressed and page aligned.
    """
    stored = name.endswith(STORED_EXTENSIONS)
    info = zipfile.ZipInfo(name)
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16

    if stored:
        header_end = archive.fp.tell() + 30 + len(name.encode("utf-8"))
        padding = (alignment - (header_end % alignment)) % alignment
        info.extra = b"\x00" * padding

    archive.writestr(info, data)


def patch(apk_path: str, loader_path: str, output_path: str, abi: str, alignment: int,
          add_permission: bool, app_label: str) -> int:
    if not zipfile.is_zipfile(apk_path):
        sys.exit(f"{apk_path} is not an APK")
    if not shutil.which("patchelf"):
        sys.exit("patchelf is required: apt install patchelf")

    loader_name = os.path.basename(loader_path)
    library_dir = f"lib/{abi}/"
    game_entry = f"{library_dir}{GAME_LIBRARY}"

    with zipfile.ZipFile(apk_path) as source:
        names = source.namelist()
        if game_entry not in names:
            available = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and "/" in n[4:]})
            sys.exit(f"{game_entry} not found. ABIs present: {', '.join(available) or 'none'}")
        if f"{library_dir}{loader_name}" in names:
            print("warning: this APK already carries a loader; it will be replaced")

        with tempfile.TemporaryDirectory() as workspace:
            game_path = os.path.join(workspace, GAME_LIBRARY)
            with open(game_path, "wb") as out:
                out.write(source.read(game_entry))

            existing = subprocess.run(
                ["patchelf", "--print-needed", game_path], capture_output=True, text=True, check=True
            ).stdout.split()
            if loader_name in existing:
                print(f"{GAME_LIBRARY} already depends on {loader_name}; rewriting anyway")
            else:
                subprocess.run(["patchelf", "--add-needed", loader_name, game_path], check=True)
            print(f"added {loader_name} as a dependency of {GAME_LIBRARY}")

            with open(game_path, "rb") as handle:
                patched_game = handle.read()
            with open(loader_path, "rb") as handle:
                loader_data = handle.read()

            written = 0
            with zipfile.ZipFile(output_path, "w") as destination:
                for info in source.infolist():
                    if info.is_dir():
                        continue
                    if info.filename.startswith("META-INF/") and info.filename.endswith(
                        (".RSA", ".SF", ".DSA", ".MF")
                    ):
                        continue  # the old signature is void now
                    if info.filename == f"{library_dir}{loader_name}":
                        continue

                    if info.filename == game_entry:
                        data = patched_game
                    else:
                        data = source.read(info.filename)

                    if info.filename == "AndroidManifest.xml":
                        if app_label:
                            data = rename_application(data, app_label)
                        try:
                            if add_permission:
                                data = grant_storage_access(data)
                        except axml.AxmlError as error:
                            print(f"manifest left untouched: {error}")
                            print("  add the permission by hand in MT Manager if the loader")
                            print(f"  reports it cannot write to /sdcard: {ALL_FILES_PERMISSION}")

                    add_entry(destination, info.filename, data, alignment)
                    written += 1

                add_entry(destination, f"{library_dir}{loader_name}", loader_data, alignment)
                written += 1

    print(f"wrote {output_path} ({written} entries)")

    signer = find_apksigner()
    if signer is None:
        print()
        print("Not signed. Android refuses unsigned APKs, so either:")
        print("  - install Android build-tools and rerun, or")
        print("  - open the file in MT Manager or APK Editor on the phone,")
        print("    which sign automatically on save.")
        return 0

    keystore = os.path.expanduser("~/.mcbeloader/debug.keystore")
    if not os.path.isfile(keystore):
        os.makedirs(os.path.dirname(keystore), exist_ok=True)
        subprocess.run(
            [
                "keytool", "-genkeypair", "-keystore", keystore, "-storepass", "android",
                "-keypass", "android", "-alias", "mcbeloader", "-keyalg", "RSA", "-keysize", "2048",
                "-validity", "10000", "-dname", "CN=mcbeloader",
            ],
            check=True,
        )
        print(f"created a signing key at {keystore}")

    subprocess.run(
        [signer, "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android",
         output_path],
        check=True,
    )
    print("signed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apk", help="the Minecraft APK to patch")
    parser.add_argument("loader", help="libmcbeloader.so built for the same ABI")
    parser.add_argument("-o", "--output", default="patched.apk")
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--name", default="NRZLoader",
                        help="name the patched app shows on the home screen; empty to keep it")
    parser.add_argument("--no-permission", action="store_true",
                        help="leave the manifest alone instead of adding all-files access")
    parser.add_argument(
        "--alignment", type=int, default=16384,
        help="payload alignment for stored entries; 16384 keeps Android 15 happy",
    )
    arguments = parser.parse_args()
    return patch(arguments.apk, arguments.loader, arguments.output, arguments.abi,
                 arguments.alignment, not arguments.no_permission, arguments.name)


if __name__ == "__main__":
    sys.exit(main())
