package ru.narezany.nrzloader.patch;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Patches a stand-in package and checks the result, including the page
 * alignment the platform needs to map a library straight out of the archive.
 *
 * Arguments: source apk, output apk, loader .so, bootstrap dex, keystore.
 */
public final class ApkPatcherTest {
    private static int checks = 0;
    private static int failures = 0;

    private static void check(boolean condition, String what) {
        checks++;
        System.out.println((condition ? "  ok    " : "  FAIL  ") + what);
        if (!condition) failures++;
    }

    public static void main(String[] args) throws Exception {
        File source = new File(args[0]);
        File output = new File(args[1]);

        ApkPatcher.Options options = new ApkPatcher.Options();
        options.loaderVersion = "1.1.0";
        options.loaderLibrary = Files.readAllBytes(Paths.get(args[2]));
        options.bootstrapDex = Files.readAllBytes(Paths.get(args[3]));
        options.applicationLabel = "NRZLoader";
        try (InputStream keystore = new FileInputStream(args[4])) {
            ApkPatcher.loadSigningKey(keystore, "nrzloader", options);
        }

        System.out.println("patching");
        ApkPatcher.Result result = ApkPatcher.patch(source, output, options,
                (message, percent) -> System.out.println("  " + percent + "%  " + message));

        check(output.isFile() && output.length() > 0, "an output package was produced");
        check(result.dexName.equals("classes2.dex"), "the bootstrap took a free dex slot");

        System.out.println("\ncontents");
        try (ZipFile patched = new ZipFile(output)) {
            check(patched.getEntry("lib/arm64-v8a/libnrzloader.so") != null, "loader present");
            check(patched.getEntry("lib/arm64-v8a/libminecraftpe.so") != null, "game library kept");
            check(patched.getEntry("classes2.dex") != null, "bootstrap classes present");
            check(patched.getEntry("classes.dex") != null, "original classes kept");
            check(patched.getEntry("assets/keep.txt") != null, "unrelated assets kept");

            // The launcher reads this back to say which loader an install
            // carries, which is what makes a mod's version requirement mean
            // anything.
            ZipEntry stamp = patched.getEntry("assets/nrzloader.version");
            check(stamp != null, "the loader version was recorded");
            if (stamp != null) {
                byte[] recorded = new byte[(int) stamp.getSize()];
                try (InputStream stream = patched.getInputStream(stamp)) {
                    int read = 0;
                    while (read < recorded.length) {
                        int step = stream.read(recorded, read, recorded.length - read);
                        if (step < 0) break;
                        read += step;
                    }
                }
                check(new String(recorded, "UTF-8").equals("1.1.0"),
                        "and it says what the patcher was told");
            }

            ZipEntry loader = patched.getEntry("lib/arm64-v8a/libnrzloader.so");
            check(loader.getMethod() == ZipEntry.STORED, "loader is stored uncompressed");
            ZipEntry game = patched.getEntry("lib/arm64-v8a/libminecraftpe.so");
            check(game.getMethod() == ZipEntry.STORED, "game library is stored uncompressed");

            byte[] original = Files.readAllBytes(Paths.get(args[2]));
            byte[] repacked = new byte[(int) loader.getSize()];
            try (InputStream stream = patched.getInputStream(loader)) {
                int read = 0;
                while (read < repacked.length) {
                    int step = stream.read(repacked, read, repacked.length - read);
                    if (step < 0) break;
                    read += step;
                }
            }
            check(java.util.Arrays.equals(original, repacked), "loader survived the repack intact");
        }

        System.out.println("\nsize");
        long grew = output.length() - source.length();
        // Padding every stored file to a page would add one per texture, which
        // on a real game runs to hundreds of megabytes of nothing.
        check(grew < 8L * 1024 * 1024,
                "output grew by " + (grew / 1024) + " KB, close to what was added");

        System.out.println("\nalignment");
        check(payloadOffset(output, "lib/arm64-v8a/libnrzloader.so") % 16384 == 0,
                "loader payload starts on a page boundary");
        check(payloadOffset(output, "lib/arm64-v8a/libminecraftpe.so") % 16384 == 0,
                "game library payload starts on a page boundary");

        System.out.println("\n" + checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }

    /**
     * Where an entry's bytes actually begin, which is what alignment is about.
     *
     * Scanned through a sliding window rather than by loading the archive: the
     * point of this test is that packages far larger than memory are handled.
     */
    private static long payloadOffset(File apk, String name) throws Exception {
        byte[] needle = name.getBytes("UTF-8");

        try (java.io.RandomAccessFile file = new java.io.RandomAccessFile(apk, "r")) {
            byte[] window = new byte[1 << 20];
            long base = 0;
            int carry = 0;

            while (true) {
                file.seek(base + carry);
                int read = file.read(window, carry, window.length - carry);
                if (read <= 0) return -1;
                int available = carry + read;

                for (int index = 0; index + 30 + needle.length <= available; index++) {
                    if (window[index] != 'P' || window[index + 1] != 'K'
                            || window[index + 2] != 3 || window[index + 3] != 4) {
                        continue;
                    }
                    int nameLength = (window[index + 26] & 0xFF) | ((window[index + 27] & 0xFF) << 8);
                    int extraLength = (window[index + 28] & 0xFF) | ((window[index + 29] & 0xFF) << 8);
                    if (nameLength != needle.length) continue;

                    boolean same = true;
                    for (int offset = 0; offset < needle.length; offset++) {
                        if (window[index + 30 + offset] != needle[offset]) {
                            same = false;
                            break;
                        }
                    }
                    if (same) return base + index + 30 + nameLength + extraLength;
                }

                // Keep enough tail to catch a header straddling the boundary.
                carry = 30 + needle.length;
                if (available <= carry) return -1;
                int keep = Math.min(carry, available);
                System.arraycopy(window, available - keep, window, 0, keep);
                base += available - keep;
                carry = keep;
            }
        }
    }
}
