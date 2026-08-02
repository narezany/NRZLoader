#!/usr/bin/env python3
"""Checks the compiled-XML editor against a manifest aapt2 really produced.

The fixture in tests/fixtures was built with aapt2 from a manifest shaped like
Minecraft's. Where androguard is installed it is used as an independent reader,
so the result is not just this code agreeing with itself.

    python3 tests/test_axml.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import axml  # noqa: E402

PERMISSION = "android.permission.MANAGE_EXTERNAL_STORAGE"
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "AndroidManifest.xml")

failures = 0
checks = 0


def check(condition, what):
    global failures, checks
    checks += 1
    print(("  ok    " if condition else "  FAIL  ") + what)
    if not condition:
        failures += 1


def independent_reader():
    """androguard, when it is available; otherwise nothing."""
    try:
        from loguru import logger

        logger.remove()
    except Exception:
        pass
    try:
        from androguard.core.axml import AXMLPrinter
    except ImportError:
        return None
    return lambda data: AXMLPrinter(data).get_xml().decode()


def main():
    with open(FIXTURE, "rb") as handle:
        original = handle.read()

    print("detection")
    check(not axml.has_permission(original, PERMISSION), "absent permission reported absent")
    check(axml.has_permission(original, "android.permission.INTERNET"), "present one detected")

    print("\npatching")
    patched = axml.add_permission(original, PERMISSION)
    check(len(patched) > len(original), "the file grew")
    check(axml.has_permission(patched, PERMISSION), "permission is now declared")
    check(axml.add_permission(patched, PERMISSION) == patched, "patching twice is a no-op")

    print("\nstructure survived")
    to_xml = independent_reader()
    if to_xml is None:
        print("  skipped: androguard not installed (pip install androguard)")
    else:
        xml = to_xml(patched)
        check(PERMISSION in xml, "an independent reader sees the permission")
        check(xml.count("<uses-permission") == 4, "all four permissions present")
        for expected in [
            "android.permission.INTERNET",
            "android.permission.ACCESS_NETWORK_STATE",
            "android.permission.VIBRATE",
            "com.mojang.minecraftpe.MainActivity",
            "android.intent.action.MAIN",
            "android.intent.category.LAUNCHER",
            'android:versionName="1.26.23.1"',
        ]:
            check(expected in xml, f"preserved: {expected}")
        check(f'<uses-permission android:name="{PERMISSION}"/>' in xml, "element is well formed")

    print("\napplication label")
    renamed = axml.set_application_label(patched, "NRZLoader")
    check(len(renamed) > len(patched), "the file grew")
    if to_xml is not None:
        xml = to_xml(renamed)
        check('android:label="NRZLoader"' in xml, "label replaced")
        check("MANAGE_EXTERNAL_STORAGE" in xml, "the permission survived the rename")
        check("com.mojang.minecraftpe.MainActivity" in xml, "the rest of the manifest survived")

    print("\nrefuses nonsense")
    for bad, description in [
        (b"", "empty input"),
        (b"not xml at all", "random bytes"),
        (original[:40], "truncated manifest"),
    ]:
        try:
            axml.add_permission(bad, PERMISSION)
            check(False, f"raises on {description}")
        except Exception:
            check(True, f"raises on {description}")

    print(f"\n{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
