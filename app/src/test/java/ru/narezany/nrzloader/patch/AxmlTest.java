package ru.narezany.nrzloader.patch;

import java.nio.file.Files;
import java.nio.file.Paths;

/**
 * Checks the manifest editor against a manifest aapt2 really produced.
 *
 * Run with a fixture path and an output directory; the output is meant to be
 * read back by an independent tool, because an editor agreeing with itself
 * proves very little.
 */
public final class AxmlTest {
    private static int checks = 0;
    private static int failures = 0;

    private static void check(boolean condition, String what) {
        checks++;
        System.out.println((condition ? "  ok    " : "  FAIL  ") + what);
        if (!condition) failures++;
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: AxmlTest <AndroidManifest.xml> <output dir>");
            System.exit(2);
        }
        byte[] original = Files.readAllBytes(Paths.get(args[0]));
        String out = args[1];

        System.out.println("detection");
        check(!Axml.hasPermission(original, Axml.ALL_FILES_PERMISSION), "absent one reported absent");
        check(Axml.hasPermission(original, "android.permission.INTERNET"), "present one detected");

        System.out.println("\npermission");
        byte[] withPermission = Axml.addPermission(original, Axml.ALL_FILES_PERMISSION);
        check(withPermission.length > original.length, "the file grew");
        check(Axml.hasPermission(withPermission, Axml.ALL_FILES_PERMISSION), "permission declared");
        check(Axml.addPermission(withPermission, Axml.ALL_FILES_PERMISSION) == withPermission,
                "adding twice is a no-op");

        // The patcher declares two, one after the other, and the second must
        // not undo the first: both go through the same string pool.
        byte[] withBoth = Axml.addPermission(withPermission, Axml.OVERLAY_PERMISSION);
        check(Axml.hasPermission(withBoth, Axml.OVERLAY_PERMISSION), "second one declared");
        check(Axml.hasPermission(withBoth, Axml.ALL_FILES_PERMISSION), "first one still there");
        check(Axml.hasPermission(withBoth, "android.permission.INTERNET"),
                "the game's own are untouched");

        System.out.println("\nlabel");
        byte[] renamed = Axml.setApplicationLabel(withBoth, "NRZLoader");
        check(renamed.length > withPermission.length, "the file grew again");

        System.out.println("\nprovider");
        byte[] withProvider = Axml.addElement(renamed, "provider", new Axml.Attribute[] {
                Axml.Attribute.ofString(Axml.ATTR_NAME, "name", "ru.narezany.nrzloader.Bootstrap"),
                Axml.Attribute.ofString(Axml.ATTR_AUTHORITIES, "authorities",
                        "com.mojang.minecraftpe.nrzloader"),
                Axml.Attribute.ofBoolean(Axml.ATTR_EXPORTED, "exported", false),
        }, Axml.Parent.APPLICATION);
        check(withProvider.length > renamed.length, "the file grew once more");

        Files.write(Paths.get(out, "with_permission.xml"), withPermission);
        Files.write(Paths.get(out, "renamed.xml"), renamed);
        Files.write(Paths.get(out, "with_provider.xml"), withProvider);

        System.out.println("\nwrote three manifests to " + out + " for independent checking");
        System.out.println("\n" + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
