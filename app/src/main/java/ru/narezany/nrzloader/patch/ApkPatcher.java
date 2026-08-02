package ru.narezany.nrzloader.patch;

import com.android.apksig.ApkSigner;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Turns a copy of the game into one that loads mods.
 *
 * Nothing from the game is redistributed: the caller supplies the package it
 * already owns, and the result is written back to the device. What gets added
 * is the loader library, a small class that loads it, and three edits to the
 * manifest.
 */
public final class ApkPatcher {
    /** Native libraries are mapped straight out of the package on modern Android. */
    private static final int PAGE_ALIGNMENT = 16384;

    private static final String LIBRARY_DIRECTORY = "lib/arm64-v8a/";
    private static final String LOADER_NAME = "libnrzloader.so";
    private static final String GAME_LIBRARY = "libminecraftpe.so";
    private static final String BOOTSTRAP_CLASS = "ru.narezany.nrzloader.Bootstrap";

    public interface Progress {
        void onStep(String message, int percent);
    }

    public static final class Result {
        public final File output;
        public final String dexName;
        public final int entries;

        Result(File output, String dexName, int entries) {
            this.output = output;
            this.dexName = dexName;
            this.entries = entries;
        }
    }

    public static final class Options {
        public byte[] loaderLibrary;
        public byte[] bootstrapDex;
        public String applicationLabel = "NRZLoader";
        public boolean requestAllFiles = true;
        /**
         * Whether mods may open their own windows over the game. The user
         * still has to allow it per app, so declaring it only makes the
         * system screen for it reachable.
         */
        public boolean allowOverlayWindows = true;
        public PrivateKey signingKey;
        public X509Certificate signingCertificate;
    }

    private ApkPatcher() {}

    /** Loads the signing key shipped with the app. */
    public static void loadSigningKey(InputStream keystore, String password, Options options)
            throws IOException {
        try {
            KeyStore store = KeyStore.getInstance("PKCS12");
            store.load(keystore, password.toCharArray());
            String alias = store.aliases().nextElement();
            options.signingKey = (PrivateKey) store.getKey(alias, password.toCharArray());
            options.signingCertificate = (X509Certificate) store.getCertificateChain(alias)[0];
        } catch (IOException error) {
            throw error;
        } catch (Exception error) {
            throw new IOException("cannot read the signing key: " + error, error);
        }
    }

    public static Result patch(File source, File output, Options options, Progress progress)
            throws IOException {
        if (options.loaderLibrary == null || options.loaderLibrary.length == 0) {
            throw new IOException("no loader library supplied");
        }
        if (options.bootstrapDex == null || options.bootstrapDex.length == 0) {
            throw new IOException("no bootstrap classes supplied");
        }

        File unsigned = new File(output.getParentFile(), output.getName() + ".unsigned");
        String dexName;
        int written;

        try (ZipFile input = new ZipFile(source)) {
            if (input.getEntry(LIBRARY_DIRECTORY + GAME_LIBRARY) == null) {
                throw new IOException("this package has no arm64 game library; a 64-bit "
                        + "Minecraft package is required");
            }
            dexName = freeDexName(input);

            step(progress, "Reading the package", 5);
            List<? extends ZipEntry> entries = Collections.list(sortedEntries(input));

            CountingStream counter = new CountingStream(new FileOutputStream(unsigned));
            try (ZipOutputStream out = new ZipOutputStream(counter)) {
                int index = 0;
                written = 0;
                for (ZipEntry entry : entries) {
                    index++;
                    if (entry.isDirectory()) continue;
                    if (isOldSignature(entry.getName())) continue;
                    if (entry.getName().equals(LIBRARY_DIRECTORY + LOADER_NAME)) continue;
                    if (entry.getName().equals(dexName)) continue;

                    if (entry.getName().equals("AndroidManifest.xml")) {
                        step(progress, "Editing the manifest", 30);
                        byte[] data = patchManifest(readAll(input.getInputStream(entry)), options);
                        writeEntry(out, counter, entry.getName(), data);
                    } else {
                        // Streamed rather than read whole: the game library alone
                        // is larger than the heap an app is allowed to grow to.
                        copyEntry(out, counter, input, entry);
                    }
                    written++;

                    if (index % 400 == 0) {
                        step(progress, "Repacking", 30 + (55 * index) / Math.max(1, entries.size()));
                    }
                }

                step(progress, "Adding the loader", 88);
                writeEntry(out, counter, LIBRARY_DIRECTORY + LOADER_NAME, options.loaderLibrary);
                writeEntry(out, counter, dexName, options.bootstrapDex);
                written += 2;
            }
        }

        step(progress, "Signing", 92);
        sign(unsigned, output, options);
        if (!unsigned.delete()) unsigned.deleteOnExit();

        step(progress, "Done", 100);
        return new Result(output, dexName, written);
    }

    // ------------------------------------------------------------------
    // Pieces
    // ------------------------------------------------------------------

    private static byte[] patchManifest(byte[] manifest, Options options) throws IOException {
        byte[] result = manifest;

        if (options.requestAllFiles) {
            result = Axml.addPermission(result, Axml.ALL_FILES_PERMISSION);
        }
        if (options.allowOverlayWindows) {
            result = Axml.addPermission(result, Axml.OVERLAY_PERMISSION);
        }
        if (options.applicationLabel != null && !options.applicationLabel.isEmpty()) {
            result = Axml.setApplicationLabel(result, options.applicationLabel);
        }

        // A content provider is created before any activity and alongside the
        // game's own Application class, so it loads the library early without
        // displacing anything the game relies on.
        return Axml.addElement(result, "provider", new Axml.Attribute[] {
                Axml.Attribute.ofString(Axml.ATTR_NAME, "name", BOOTSTRAP_CLASS),
                Axml.Attribute.ofString(Axml.ATTR_AUTHORITIES, "authorities",
                        "nrzloader.bootstrap"),
                Axml.Attribute.ofBoolean(Axml.ATTR_EXPORTED, "exported", false),
        }, Axml.Parent.APPLICATION);
    }

    /** Android loads every classes*.dex, so the first unused name will do. */
    private static String freeDexName(ZipFile input) {
        for (int index = 2; index < 100; index++) {
            String candidate = "classes" + index + ".dex";
            if (input.getEntry(candidate) == null) return candidate;
        }
        return "classes99.dex";
    }

    private static boolean isOldSignature(String name) {
        if (!name.startsWith("META-INF/")) return false;
        String upper = name.toUpperCase();
        return upper.endsWith(".RSA") || upper.endsWith(".DSA") || upper.endsWith(".EC")
                || upper.endsWith(".SF") || upper.endsWith("MANIFEST.MF");
    }

    /**
     * Only native libraries are mapped out of the package, and only they need
     * to start on a page boundary. Padding everything stored would add up to a
     * page per file, which on a game carrying thousands of textures and sounds
     * means hundreds of megabytes of nothing.
     */
    private static int alignmentFor(String name) {
        if (name.endsWith(".so")) return PAGE_ALIGNMENT;
        // The resource table is read through a memory mapping too, but a word
        // is all it asks for.
        if (name.endsWith(".arsc")) return 4;
        return 1;
    }

    /**
     * Writes one entry, padding so the payload of an uncompressed one starts on
     * a page boundary. Native libraries are mapped from the package rather than
     * unpacked, and that only works when they are stored and aligned.
     */
    private static void writeEntry(ZipOutputStream out, CountingStream counter, String name,
                                   byte[] data) throws IOException {
        boolean stored = alignmentFor(name) > 1;
        ZipEntry entry = new ZipEntry(name);

        if (stored) {
            entry.setMethod(ZipEntry.STORED);
            entry.setSize(data.length);
            entry.setCompressedSize(data.length);
            CRC32 crc = new CRC32();
            crc.update(data, 0, data.length);
            entry.setCrc(crc.getValue());

            // The stream writes the local header before the payload, so the
            // padding has to go in the extra field, whose length is part of
            // that header.
            int alignment = alignmentFor(name);
            long headerEnd = counter.count + 30 + name.getBytes("UTF-8").length;
            int padding = (int) ((alignment - (headerEnd % alignment)) % alignment);
            if (padding > 0) entry.setExtra(new byte[padding]);
        } else {
            entry.setMethod(ZipEntry.DEFLATED);
        }

        out.putNextEntry(entry);
        out.write(data);
        out.closeEntry();
    }

    private static Enumeration<? extends ZipEntry> sortedEntries(ZipFile input) {
        return input.entries();
    }

    /**
     * Copies one entry across without holding it in memory.
     *
     * The uncompressed size and checksum are already recorded in the source
     * archive's directory, which is what makes it possible to declare a stored
     * entry before its bytes have been seen.
     */
    private static void copyEntry(ZipOutputStream out, CountingStream counter, ZipFile input,
                                  ZipEntry entry) throws IOException {
        String name = entry.getName();
        // How the game packed a file is the right answer for the copy too:
        // whatever the platform needs mapped, the original already stores.
        boolean stored = entry.getMethod() == ZipEntry.STORED || alignmentFor(name) > 1;
        long size = entry.getSize();
        long crc = entry.getCrc();

        if (stored && (size < 0 || crc < 0)) {
            // Without those two the entry cannot be declared up front; falling
            // back costs memory but only for entries that lack the metadata.
            writeEntry(out, counter, name, readAll(input.getInputStream(entry)));
            return;
        }

        ZipEntry copy = new ZipEntry(name);
        if (stored) {
            copy.setMethod(ZipEntry.STORED);
            copy.setSize(size);
            copy.setCompressedSize(size);
            copy.setCrc(crc);

            int alignment = alignmentFor(name);
            long headerEnd = counter.count + 30 + name.getBytes("UTF-8").length;
            int padding = (int) ((alignment - (headerEnd % alignment)) % alignment);
            if (padding > 0) copy.setExtra(new byte[padding]);
        } else {
            copy.setMethod(ZipEntry.DEFLATED);
        }

        out.putNextEntry(copy);
        try (InputStream source = input.getInputStream(entry)) {
            byte[] chunk = new byte[262144];
            int read;
            while ((read = source.read(chunk)) > 0) out.write(chunk, 0, read);
        }
        out.closeEntry();
    }

    private static byte[] readAll(InputStream stream) throws IOException {
        try (InputStream input = stream) {
            ByteArrayOutputStream buffer = new ByteArrayOutputStream(Math.max(1024,
                    input.available()));
            byte[] chunk = new byte[65536];
            int read;
            while ((read = input.read(chunk)) > 0) buffer.write(chunk, 0, read);
            return buffer.toByteArray();
        }
    }

    private static void sign(File input, File output, Options options) throws IOException {
        if (options.signingKey == null || options.signingCertificate == null) {
            throw new IOException("no signing key; the package would not install");
        }

        ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder(
                "NRZLoader", options.signingKey,
                Collections.singletonList(options.signingCertificate)).build();

        ApkSigner.Builder builder = new ApkSigner.Builder(new ArrayList<>(
                Collections.singletonList(signer)))
                .setInputApk(input)
                .setOutputApk(output)
                // v1 is a per-entry digest over the whole archive, which on a
                // package this size costs minutes for nothing: every Android
                // version that runs the game verifies v2.
                .setV1SigningEnabled(false)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                // The alignment written above must survive; letting the signer
                // redo it would undo the page alignment.
                .setAlignmentPreserved(true);

        try {
            builder.build().sign();
        } catch (Exception error) {
            throw new IOException("signing failed: " + error, error);
        }
    }

    private static void step(Progress progress, String message, int percent) {
        if (progress != null) progress.onStep(message, percent);
    }

    /**
     * Tracks how many bytes a ZipOutputStream has produced, which the class
     * itself does not report but alignment depends on.
     */
    static final class CountingStream extends OutputStream {
        private final OutputStream delegate;
        long count;

        CountingStream(OutputStream delegate) {
            this.delegate = delegate;
        }

        @Override
        public void write(int value) throws IOException {
            delegate.write(value);
            count++;
        }

        @Override
        public void write(byte[] data, int offset, int length) throws IOException {
            delegate.write(data, offset, length);
            count += length;
        }

        @Override
        public void flush() throws IOException {
            delegate.flush();
        }

        @Override
        public void close() throws IOException {
            delegate.close();
        }
    }
}
