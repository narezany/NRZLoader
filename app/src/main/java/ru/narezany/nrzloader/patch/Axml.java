package ru.narezany.nrzloader.patch;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Editor for Android's compiled XML.
 *
 * AndroidManifest.xml inside an APK is a chunked binary format with a shared
 * string pool rather than text. Only the operations the patcher needs are
 * implemented, and anything unexpected throws instead of guessing: a silently
 * corrupted manifest produces an app that installs and then dies.
 */
public final class Axml {
    private static final int CHUNK_XML = 0x0003;
    private static final int CHUNK_STRING_POOL = 0x0001;
    private static final int CHUNK_RESOURCE_MAP = 0x0180;
    private static final int CHUNK_START_ELEMENT = 0x0102;
    private static final int CHUNK_END_ELEMENT = 0x0103;

    private static final int UTF8_FLAG = 1 << 8;
    private static final int TYPE_STRING = 0x03;
    private static final int TYPE_BOOLEAN = 0x12;

    private static final String ANDROID_NAMESPACE = "http://schemas.android.com/apk/res/android";

    public static final int ATTR_NAME = 0x01010003;
    public static final int ATTR_LABEL = 0x01010001;
    public static final int ATTR_AUTHORITIES = 0x01010018;
    public static final int ATTR_EXPORTED = 0x01010010;

    public static final String ALL_FILES_PERMISSION = "android.permission.MANAGE_EXTERNAL_STORAGE";
    /** Lets a mod put its own window on top of the game. */
    public static final String OVERLAY_PERMISSION = "android.permission.SYSTEM_ALERT_WINDOW";

    public static class AxmlException extends IOException {
        AxmlException(String message) {
            super(message);
        }
    }

    private Axml() {}

    // ---------------------------------------------------------------------
    // String pool
    // ---------------------------------------------------------------------

    private static final class StringPool {
        final List<String> strings = new ArrayList<>();
        final List<Integer> offsets = new ArrayList<>();
        final List<Integer> styleOffsets = new ArrayList<>();
        byte[] data;
        byte[] styleData = new byte[0];
        int flags;
        boolean utf8;

        StringPool(byte[] chunk) throws AxmlException {
            ByteBuffer buffer = wrap(chunk);
            int kind = buffer.getShort(0) & 0xFFFF;
            if (kind != CHUNK_STRING_POOL) throw new AxmlException("not a string pool");

            int headerSize = buffer.getShort(2) & 0xFFFF;
            int size = buffer.getInt(4);
            int stringCount = buffer.getInt(8);
            int styleCount = buffer.getInt(12);
            flags = buffer.getInt(16);
            int stringsStart = buffer.getInt(20);
            int stylesStart = buffer.getInt(24);
            utf8 = (flags & UTF8_FLAG) != 0;

            for (int index = 0; index < stringCount; index++) {
                offsets.add(buffer.getInt(headerSize + index * 4));
            }
            for (int index = 0; index < styleCount; index++) {
                styleOffsets.add(buffer.getInt(headerSize + stringCount * 4 + index * 4));
            }

            int end = (styleCount > 0 && stylesStart > 0) ? stylesStart : size;
            data = new byte[end - stringsStart];
            System.arraycopy(chunk, stringsStart, data, 0, data.length);
            if (styleCount > 0 && stylesStart > 0) {
                styleData = new byte[size - stylesStart];
                System.arraycopy(chunk, stylesStart, styleData, 0, styleData.length);
            }

            // Entries are reached only through the offset table and equal
            // strings are commonly folded together, so the block is never
            // walked from front to back.
            for (int offset : offsets) strings.add(read(offset));
        }

        private String read(int offset) {
            if (utf8) {
                int[] cursor = {offset};
                decodeLength8(cursor);
                int byteLength = decodeLength8(cursor);
                return new String(data, cursor[0], byteLength, StandardCharsets.UTF_8);
            }
            int[] cursor = {offset};
            int length = decodeLength16(cursor);
            return new String(data, cursor[0], length * 2, StandardCharsets.UTF_16LE);
        }

        private int decodeLength8(int[] cursor) {
            int first = data[cursor[0]] & 0xFF;
            if ((first & 0x80) != 0) {
                int value = ((first & 0x7F) << 8) | (data[cursor[0] + 1] & 0xFF);
                cursor[0] += 2;
                return value;
            }
            cursor[0] += 1;
            return first;
        }

        private int decodeLength16(int[] cursor) {
            int first = (data[cursor[0]] & 0xFF) | ((data[cursor[0] + 1] & 0xFF) << 8);
            if ((first & 0x8000) != 0) {
                int second = (data[cursor[0] + 2] & 0xFF) | ((data[cursor[0] + 3] & 0xFF) << 8);
                cursor[0] += 4;
                return ((first & 0x7FFF) << 16) | second;
            }
            cursor[0] += 2;
            return first;
        }

        int indexOf(String text) {
            return strings.indexOf(text);
        }

        /**
         * Appends even when the text is already present. An attribute name has
         * to sit at the index the resource map assigns it, so an existing copy
         * used for something else cannot be shared.
         */
        int addForced(String text) {
            return append(text);
        }

        /** Appends a string; every existing offset keeps pointing at the same text. */
        int add(String text) {
            int existing = indexOf(text);
            if (existing >= 0) return existing;
            return append(text);
        }

        private int append(String text) {
            byte[] encoded;
            if (utf8) {
                byte[] payload = text.getBytes(StandardCharsets.UTF_8);
                byte[] charCount = encodeLength8(text.length());
                byte[] byteCount = encodeLength8(payload.length);
                encoded = new byte[charCount.length + byteCount.length + payload.length + 1];
                int cursor = 0;
                System.arraycopy(charCount, 0, encoded, cursor, charCount.length);
                cursor += charCount.length;
                System.arraycopy(byteCount, 0, encoded, cursor, byteCount.length);
                cursor += byteCount.length;
                System.arraycopy(payload, 0, encoded, cursor, payload.length);
            } else {
                byte[] payload = text.getBytes(StandardCharsets.UTF_16LE);
                encoded = new byte[2 + payload.length + 2];
                encoded[0] = (byte) (text.length() & 0xFF);
                encoded[1] = (byte) ((text.length() >> 8) & 0xFF);
                System.arraycopy(payload, 0, encoded, 2, payload.length);
            }

            int offset = data.length;
            byte[] grown = new byte[data.length + encoded.length];
            System.arraycopy(data, 0, grown, 0, data.length);
            System.arraycopy(encoded, 0, grown, data.length, encoded.length);
            data = grown;

            offsets.add(offset);
            strings.add(text);
            return strings.size() - 1;
        }

        private static byte[] encodeLength8(int value) {
            if (value < 0x80) return new byte[] {(byte) value};
            return new byte[] {(byte) ((value >> 8) | 0x80), (byte) (value & 0xFF)};
        }

        byte[] build() {
            int padding = (4 - data.length % 4) % 4;
            int headerSize = 28;
            int stringsStart = headerSize + 4 * offsets.size() + 4 * styleOffsets.size();
            int paddedLength = data.length + padding;
            int stylesStart = styleData.length > 0 ? stringsStart + paddedLength : 0;
            int total = stringsStart + paddedLength + styleData.length;

            ByteBuffer out = ByteBuffer.allocate(total).order(ByteOrder.LITTLE_ENDIAN);
            out.putShort((short) CHUNK_STRING_POOL);
            out.putShort((short) headerSize);
            out.putInt(total);
            out.putInt(strings.size());
            out.putInt(styleOffsets.size());
            out.putInt(flags);
            out.putInt(stringsStart);
            out.putInt(stylesStart);
            for (int offset : offsets) out.putInt(offset);
            for (int offset : styleOffsets) out.putInt(offset);
            out.put(data);
            out.put(new byte[padding]);
            out.put(styleData);
            return out.array();
        }
    }

    // ---------------------------------------------------------------------
    // Chunk handling
    // ---------------------------------------------------------------------

    private static ByteBuffer wrap(byte[] data) {
        return ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static final class Chunk {
        final int kind;
        byte[] bytes;

        Chunk(int kind, byte[] bytes) {
            this.kind = kind;
            this.bytes = bytes;
        }
    }

    private static List<Chunk> split(byte[] body) throws AxmlException {
        List<Chunk> chunks = new ArrayList<>();
        ByteBuffer buffer = wrap(body);
        int cursor = 0;
        while (cursor + 8 <= body.length) {
            int kind = buffer.getShort(cursor) & 0xFFFF;
            int size = buffer.getInt(cursor + 4);
            if (size < 8 || cursor + size > body.length) {
                throw new AxmlException("chunk 0x" + Integer.toHexString(kind) + " has a bad size");
            }
            byte[] slice = new byte[size];
            System.arraycopy(body, cursor, slice, 0, size);
            chunks.add(new Chunk(kind, slice));
            cursor += size;
        }
        if (cursor != body.length) throw new AxmlException("trailing bytes after the last chunk");
        return chunks;
    }

    private static byte[] assemble(int headerSize, List<Chunk> chunks) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        for (Chunk chunk : chunks) body.write(chunk.bytes, 0, chunk.bytes.length);

        byte[] payload = body.toByteArray();
        ByteBuffer out = ByteBuffer.allocate(headerSize + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        out.putShort((short) CHUNK_XML);
        out.putShort((short) headerSize);
        out.putInt(headerSize + payload.length);
        out.put(payload);
        return out.array();
    }

    private static int[] resourceIds(List<Chunk> chunks) throws AxmlException {
        for (Chunk chunk : chunks) {
            if (chunk.kind != CHUNK_RESOURCE_MAP) continue;
            int count = (chunk.bytes.length - 8) / 4;
            ByteBuffer buffer = wrap(chunk.bytes);
            int[] ids = new int[count];
            for (int index = 0; index < count; index++) ids[index] = buffer.getInt(8 + index * 4);
            return ids;
        }
        throw new AxmlException("no resource map; this is not a manifest aapt produced");
    }

    private static int attributeNameIndex(int[] ids, int resourceId, String what)
            throws AxmlException {
        for (int index = 0; index < ids.length; index++) {
            if (ids[index] == resourceId) return index;
        }
        throw new AxmlException("manifest never uses android:" + what);
    }

    /**
     * Index of the string the resource map ties to `resourceId`, creating both
     * the string and the mapping when the manifest has never used that
     * attribute. Without the mapping the platform reads the attribute as
     * nameless and ignores it.
     */
    private static int ensureAttributeName(StringPool pool, List<Integer> ids, int resourceId,
                                           String name) {
        for (int index = 0; index < ids.size(); index++) {
            if (ids.get(index) == resourceId) return index;
        }

        int index = pool.addForced(name);
        while (ids.size() < index) ids.add(0);
        ids.add(resourceId);
        return index;
    }

    private static byte[] buildResourceMap(List<Integer> ids) {
        int total = 8 + ids.size() * 4;
        ByteBuffer out = ByteBuffer.allocate(total).order(ByteOrder.LITTLE_ENDIAN);
        out.putShort((short) CHUNK_RESOURCE_MAP);
        out.putShort((short) 8);
        out.putInt(total);
        for (int id : ids) out.putInt(id);
        return out.array();
    }

    /** One attribute of an element being built. */
    public static final class Attribute {
        final int resourceId;
        final String attributeName;
        final String stringValue;   // null for a boolean
        final boolean booleanValue;

        public static Attribute ofString(int resourceId, String name, String value) {
            return new Attribute(resourceId, name, value, false);
        }

        public static Attribute ofBoolean(int resourceId, String name, boolean value) {
            return new Attribute(resourceId, name, null, value);
        }

        private Attribute(int resourceId, String name, String value, boolean booleanValue) {
            this.resourceId = resourceId;
            this.attributeName = name;
            this.stringValue = value;
            this.booleanValue = booleanValue;
        }
    }

    // ---------------------------------------------------------------------
    // Public operations
    // ---------------------------------------------------------------------

    public static boolean hasPermission(byte[] axml, String permission) {
        try {
            ByteBuffer buffer = wrap(axml);
            if ((buffer.getShort(0) & 0xFFFF) != CHUNK_XML) return false;
            int headerSize = buffer.getShort(2) & 0xFFFF;

            byte[] body = new byte[axml.length - headerSize];
            System.arraycopy(axml, headerSize, body, 0, body.length);
            List<Chunk> chunks = split(body);
            StringPool pool = new StringPool(chunks.get(0).bytes);

            int permissionIndex = pool.indexOf(permission);
            int elementIndex = pool.indexOf("uses-permission");
            if (permissionIndex < 0 || elementIndex < 0) return false;

            for (Chunk chunk : chunks) {
                if (chunk.kind != CHUNK_START_ELEMENT) continue;
                ByteBuffer element = wrap(chunk.bytes);
                if (element.getInt(20) != elementIndex) continue;
                int count = element.getShort(28) & 0xFFFF;
                for (int index = 0; index < count; index++) {
                    if (element.getInt(36 + index * 20 + 8) == permissionIndex) return true;
                }
            }
            return false;
        } catch (IOException | RuntimeException error) {
            return false;
        }
    }

    /** Adds a permission, or returns the input untouched when it is already there. */
    public static byte[] addPermission(byte[] axml, String permission) throws IOException {
        if (hasPermission(axml, permission)) return axml;
        return addElement(axml, "uses-permission",
                new Attribute[] {Attribute.ofString(ATTR_NAME, "name", permission)});
    }

    /** Where a new element belongs. */
    public enum Parent {
        /** Directly under &lt;manifest&gt;, where uses-permission lives. */
        MANIFEST,
        /** Inside &lt;application&gt;, which is the only valid place for a provider. */
        APPLICATION,
    }

    public static byte[] addElement(byte[] axml, String elementName, Attribute[] attributes)
            throws IOException {
        return addElement(axml, elementName, attributes, Parent.MANIFEST);
    }

    /** Inserts an element as the first child of the chosen parent. */
    public static byte[] addElement(byte[] axml, String elementName, Attribute[] attributes,
                                    Parent parent) throws IOException {
        ByteBuffer header = wrap(axml);
        if ((header.getShort(0) & 0xFFFF) != CHUNK_XML) throw new AxmlException("not compiled XML");
        int headerSize = header.getShort(2) & 0xFFFF;
        if (header.getInt(4) != axml.length) throw new AxmlException("declared size is wrong");

        byte[] body = new byte[axml.length - headerSize];
        System.arraycopy(axml, headerSize, body, 0, body.length);
        List<Chunk> chunks = split(body);
        if (chunks.isEmpty() || chunks.get(0).kind != CHUNK_STRING_POOL) {
            throw new AxmlException("expected a string pool first");
        }

        StringPool pool = new StringPool(chunks.get(0).bytes);

        List<Integer> ids = new ArrayList<>();
        for (int id : resourceIds(chunks)) ids.add(id);

        int namespaceIndex = pool.indexOf(ANDROID_NAMESPACE);
        if (namespaceIndex < 0) throw new AxmlException("android namespace missing");

        // Attribute names first: they must land at the indices the resource map
        // assigns, before any other string is appended after them.
        int[] nameIndices = new int[attributes.length];
        for (int index = 0; index < attributes.length; index++) {
            Attribute attribute = attributes[index];
            nameIndices[index] =
                    ensureAttributeName(pool, ids, attribute.resourceId, attribute.attributeName);
        }

        int elementIndex = pool.add(elementName);

        int[] valueIndices = new int[attributes.length];
        for (int index = 0; index < attributes.length; index++) {
            valueIndices[index] = attributes[index].stringValue == null
                    ? -1
                    : pool.add(attributes[index].stringValue);
        }

        int startSize = 36 + attributes.length * 20;
        ByteBuffer start = ByteBuffer.allocate(startSize).order(ByteOrder.LITTLE_ENDIAN);
        start.putShort((short) CHUNK_START_ELEMENT);
        start.putShort((short) 16);
        start.putInt(startSize);
        start.putInt(-1);  // line number
        start.putInt(-1);  // comment
        start.putInt(-1);  // namespace
        start.putInt(elementIndex);
        start.putShort((short) 20);                       // attribute start
        start.putShort((short) 20);                       // attribute size
        start.putShort((short) attributes.length);
        start.putShort((short) 0);                        // id index
        start.putShort((short) 0);                        // class index
        start.putShort((short) 0);                        // style index
        for (int index = 0; index < attributes.length; index++) {
            Attribute attribute = attributes[index];
            start.putInt(namespaceIndex);
            start.putInt(nameIndices[index]);
            if (attribute.stringValue != null) {
                start.putInt(valueIndices[index]);
                start.putShort((short) 8);
                start.put((byte) 0);
                start.put((byte) TYPE_STRING);
                start.putInt(valueIndices[index]);
            } else {
                start.putInt(-1);  // no raw value for a typed boolean
                start.putShort((short) 8);
                start.put((byte) 0);
                start.put((byte) TYPE_BOOLEAN);
                start.putInt(attribute.booleanValue ? -1 : 0);
            }
        }

        ByteBuffer end = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        end.putShort((short) CHUNK_END_ELEMENT);
        end.putShort((short) 16);
        end.putInt(24);
        end.putInt(-1);
        end.putInt(-1);
        end.putInt(-1);
        end.putInt(elementIndex);

        // A provider outside <application> is silently ignored by the platform,
        // so the parent matters as much as the element itself.
        int applicationIndex = pool.indexOf("application");
        if (parent == Parent.APPLICATION && applicationIndex < 0) {
            throw new AxmlException("no application element to insert into");
        }

        List<Chunk> rebuilt = new ArrayList<>();
        boolean inserted = false;
        for (Chunk chunk : chunks) {
            if (chunk.kind == CHUNK_STRING_POOL) {
                rebuilt.add(new Chunk(CHUNK_STRING_POOL, pool.build()));
                continue;
            }
            if (chunk.kind == CHUNK_RESOURCE_MAP) {
                rebuilt.add(new Chunk(CHUNK_RESOURCE_MAP, buildResourceMap(ids)));
                continue;
            }

            rebuilt.add(chunk);
            if (inserted || chunk.kind != CHUNK_START_ELEMENT) continue;

            boolean here = parent == Parent.MANIFEST
                    || wrap(chunk.bytes).getInt(20) == applicationIndex;
            if (here) {
                rebuilt.add(new Chunk(CHUNK_START_ELEMENT, start.array()));
                rebuilt.add(new Chunk(CHUNK_END_ELEMENT, end.array()));
                inserted = true;
            }
        }
        if (!inserted) throw new AxmlException("no element to attach to");

        return assemble(headerSize, rebuilt);
    }

    /**
     * Renames the app as it appears on the home screen. The label is normally a
     * reference to a string resource; this replaces it with a literal, which
     * needs no matching entry in the resource table.
     */
    public static byte[] setApplicationLabel(byte[] axml, String label) throws IOException {
        ByteBuffer header = wrap(axml);
        if ((header.getShort(0) & 0xFFFF) != CHUNK_XML) throw new AxmlException("not compiled XML");
        int headerSize = header.getShort(2) & 0xFFFF;

        byte[] body = new byte[axml.length - headerSize];
        System.arraycopy(axml, headerSize, body, 0, body.length);
        List<Chunk> chunks = split(body);
        StringPool pool = new StringPool(chunks.get(0).bytes);
        int[] ids = resourceIds(chunks);

        int labelNameIndex = attributeNameIndex(ids, ATTR_LABEL, "label");
        int applicationIndex = pool.indexOf("application");
        if (applicationIndex < 0) throw new AxmlException("no application element");

        int labelIndex = pool.add(label);

        boolean changed = false;
        List<Chunk> rebuilt = new ArrayList<>();
        for (Chunk chunk : chunks) {
            if (chunk.kind == CHUNK_STRING_POOL) {
                rebuilt.add(new Chunk(CHUNK_STRING_POOL, pool.build()));
                continue;
            }
            if (chunk.kind == CHUNK_START_ELEMENT && !changed) {
                ByteBuffer element = wrap(chunk.bytes);
                if (element.getInt(20) == applicationIndex) {
                    byte[] editable = chunk.bytes.clone();
                    ByteBuffer view = wrap(editable);
                    int count = view.getShort(28) & 0xFFFF;
                    for (int index = 0; index < count; index++) {
                        int base = 36 + index * 20;
                        if (view.getInt(base + 4) != labelNameIndex) continue;
                        view.putInt(base + 8, labelIndex);
                        view.putShort(base + 12, (short) 8);
                        editable[base + 14] = 0;
                        editable[base + 15] = (byte) TYPE_STRING;
                        view.putInt(base + 16, labelIndex);
                        changed = true;
                        break;
                    }
                    rebuilt.add(new Chunk(chunk.kind, editable));
                    continue;
                }
            }
            rebuilt.add(chunk);
        }
        if (!changed) throw new AxmlException("the application element has no android:label");

        return assemble(headerSize, rebuilt);
    }
}
