"""Minimal editor for Android's compiled XML.

AndroidManifest.xml inside an APK is not text: it is a chunked binary format
with a shared string pool. Adding a permission means appending a string to
that pool and splicing in a start/end element pair, then fixing the sizes.

Only what the loader needs is implemented. Anything unexpected raises rather
than guessing, because a silently corrupted manifest produces an app that
installs and then dies.
"""

from __future__ import annotations

import struct

CHUNK_XML = 0x0003
CHUNK_STRING_POOL = 0x0001
CHUNK_RESOURCE_MAP = 0x0180
CHUNK_START_NAMESPACE = 0x0100
CHUNK_END_NAMESPACE = 0x0101
CHUNK_START_ELEMENT = 0x0102
CHUNK_END_ELEMENT = 0x0103

UTF8_FLAG = 1 << 8
TYPE_STRING = 0x03
ANDROID_NAMESPACE = "http://schemas.android.com/apk/res/android"
ATTR_NAME_RESOURCE_ID = 0x01010003


class AxmlError(Exception):
    pass


def _encode_length_utf8(value: int) -> bytes:
    if value > 0x7FFF:
        raise AxmlError("string too long")
    if value < 0x80:
        return bytes([value])
    return bytes([(value >> 8) | 0x80, value & 0xFF])


def _encode_length_utf16(value: int) -> bytes:
    if value > 0x7FFF:
        raise AxmlError("string too long")
    return struct.pack("<H", value)


def _decode_length_utf8(data: bytes, offset: int) -> tuple[int, int]:
    first = data[offset]
    if first & 0x80:
        return ((first & 0x7F) << 8) | data[offset + 1], offset + 2
    return first, offset + 1


def _decode_length_utf16(data: bytes, offset: int) -> tuple[int, int]:
    first, = struct.unpack_from("<H", data, offset)
    if first & 0x8000:
        second, = struct.unpack_from("<H", data, offset + 2)
        return ((first & 0x7FFF) << 16) | second, offset + 4
    return first, offset + 2


class StringPool:
    def __init__(self, chunk: bytes):
        kind, header_size, size = struct.unpack_from("<HHI", chunk, 0)
        if kind != CHUNK_STRING_POOL:
            raise AxmlError("not a string pool")

        (self.string_count, self.style_count, self.flags, strings_start,
         styles_start) = struct.unpack_from("<IIIII", chunk, 8)
        self.utf8 = bool(self.flags & UTF8_FLAG)

        self.offsets = list(struct.unpack_from(f"<{self.string_count}I", chunk, header_size))
        self.style_offsets = list(
            struct.unpack_from(f"<{self.style_count}I", chunk, header_size + 4 * self.string_count)
        ) if self.style_count else []

        end = styles_start if self.style_count and styles_start else size
        self.string_data = bytearray(chunk[strings_start:end])
        self.style_data = bytearray(chunk[styles_start:size]) if self.style_count else bytearray()

        # Entries are addressed purely through the offset table, and identical
        # strings are commonly folded onto one another, so the data block is
        # never walked sequentially.
        self.strings = [self._read(offset) for offset in self.offsets]

    def _read(self, offset: int) -> str:
        data = self.string_data
        if self.utf8:
            _, cursor = _decode_length_utf8(data, offset)
            byte_length, cursor = _decode_length_utf8(data, cursor)
            return data[cursor:cursor + byte_length].decode("utf-8", "replace")
        length, cursor = _decode_length_utf16(data, offset)
        return data[cursor:cursor + length * 2].decode("utf-16-le", "replace")

    def index_of(self, text: str) -> int:
        try:
            return self.strings.index(text)
        except ValueError:
            return -1

    def add(self, text: str) -> int:
        """Appends a string. Existing indices keep pointing at the same text."""
        existing = self.index_of(text)
        if existing >= 0:
            return existing

        if self.utf8:
            payload = text.encode("utf-8")
            encoded = (_encode_length_utf8(len(text.encode("utf-16-le")) // 2)
                       + _encode_length_utf8(len(payload)) + payload + b"\x00")
        else:
            payload = text.encode("utf-16-le")
            encoded = _encode_length_utf16(len(payload) // 2) + payload + b"\x00\x00"

        # Appending keeps every existing offset valid, which is what makes it
        # safe to leave the rest of the file untouched.
        offset = len(self.string_data)
        self.string_data.extend(encoded)
        self.offsets.append(offset)
        self.strings.append(text)
        return len(self.strings) - 1

    def build(self) -> bytes:
        offsets = self.offsets
        data = bytes(self.string_data)
        if len(offsets) != len(self.strings):
            raise AxmlError("offset table and string list disagree")

        padding = (4 - len(data) % 4) % 4
        data += b"\x00" * padding

        header_size = 28
        strings_start = header_size + 4 * len(self.strings) + 4 * len(self.style_offsets)
        styles_start = strings_start + len(data) if self.style_data else 0
        total = strings_start + len(data) + len(self.style_data)

        out = bytearray()
        out += struct.pack("<HHI", CHUNK_STRING_POOL, header_size, total)
        out += struct.pack("<IIIII", len(self.strings), len(self.style_offsets), self.flags,
                           strings_start, styles_start)
        out += struct.pack(f"<{len(offsets)}I", *offsets) if offsets else b""
        out += struct.pack(f"<{len(self.style_offsets)}I", *self.style_offsets) if self.style_offsets else b""
        out += data
        out += bytes(self.style_data)
        return bytes(out)


def _split_chunks(body: bytes) -> list[tuple[int, bytes]]:
    chunks = []
    cursor = 0
    while cursor + 8 <= len(body):
        kind, _header_size, size = struct.unpack_from("<HHI", body, cursor)
        if size < 8 or cursor + size > len(body):
            raise AxmlError(f"chunk 0x{kind:04x} has an impossible size")
        chunks.append((kind, body[cursor:cursor + size]))
        cursor += size
    if cursor != len(body):
        raise AxmlError("trailing bytes after the last chunk")
    return chunks


def has_permission(axml: bytes, permission: str) -> bool:
    """True when the manifest already declares the permission."""
    try:
        kind, header_size, _size = struct.unpack_from("<HHI", axml, 0)
        if kind != CHUNK_XML:
            return False
        chunks = _split_chunks(axml[header_size:])
        pool = StringPool(chunks[0][1])
    except (AxmlError, struct.error, IndexError):
        return False

    if pool.index_of(permission) < 0:
        return False

    # The string being present is not proof it is used as a permission, so
    # check that a uses-permission element actually references it.
    permission_index = pool.index_of(permission)
    element_index = pool.index_of("uses-permission")
    if element_index < 0:
        return False

    for chunk_kind, chunk in chunks:
        if chunk_kind != CHUNK_START_ELEMENT:
            continue
        _ns, name = struct.unpack_from("<II", chunk, 16)
        if name != element_index:
            continue
        count, = struct.unpack_from("<H", chunk, 28)
        for index in range(count):
            base = 36 + index * 20
            _attr_ns, _attr_name, raw_value = struct.unpack_from("<III", chunk, base)
            if raw_value == permission_index:
                return True
    return False


ATTR_LABEL_RESOURCE_ID = 0x01010001


def set_application_label(axml: bytes, label: str) -> bytes:
    """Renames the app as it appears on the home screen.

    The label is usually a reference to a string resource; this replaces it
    with a literal, which needs no matching entry in resources.arsc.
    """
    kind, header_size, size = struct.unpack_from("<HHI", axml, 0)
    if kind != CHUNK_XML:
        raise AxmlError("not a compiled XML file")
    if size != len(axml):
        raise AxmlError("declared size does not match the file")

    chunks = _split_chunks(axml[header_size:])
    if not chunks or chunks[0][0] != CHUNK_STRING_POOL:
        raise AxmlError("expected a string pool first")

    pool = StringPool(chunks[0][1])

    resource_map = next((chunk for chunk_kind, chunk in chunks if chunk_kind == CHUNK_RESOURCE_MAP),
                        None)
    if resource_map is None:
        raise AxmlError("no resource map")

    resource_ids = list(struct.unpack_from(f"<{(len(resource_map) - 8) // 4}I", resource_map, 8))
    try:
        label_name_index = resource_ids.index(ATTR_LABEL_RESOURCE_ID)
    except ValueError as error:
        raise AxmlError("manifest never uses android:label") from error

    application_index = pool.index_of("application")
    if application_index < 0:
        raise AxmlError("no application element")

    label_index = pool.add(label)

    rebuilt = bytearray()
    changed = False
    for chunk_kind, chunk in chunks:
        if chunk_kind == CHUNK_STRING_POOL:
            rebuilt += pool.build()
            continue

        if chunk_kind == CHUNK_START_ELEMENT and not changed:
            _ns, name = struct.unpack_from("<II", chunk, 16)
            if name == application_index:
                editable = bytearray(chunk)
                count, = struct.unpack_from("<H", editable, 28)
                for index in range(count):
                    base = 36 + index * 20
                    _attr_ns, attr_name = struct.unpack_from("<II", editable, base)
                    if attr_name != label_name_index:
                        continue
                    # rawValue, then the typed value: size, res0, type, data.
                    struct.pack_into("<I", editable, base + 8, label_index)
                    struct.pack_into("<HBBI", editable, base + 12, 8, 0, TYPE_STRING, label_index)
                    changed = True
                    break
                rebuilt += editable
                continue

        rebuilt += chunk

    if not changed:
        raise AxmlError("the application element has no android:label to replace")

    return struct.pack("<HHI", CHUNK_XML, header_size, header_size + len(rebuilt)) + bytes(rebuilt)


def add_permission(axml: bytes, permission: str) -> bytes:
    """Returns a manifest with `permission` declared, or the input unchanged."""
    if has_permission(axml, permission):
        return axml

    kind, header_size, size = struct.unpack_from("<HHI", axml, 0)
    if kind != CHUNK_XML:
        raise AxmlError("not a compiled XML file")
    if size != len(axml):
        raise AxmlError("declared size does not match the file")

    chunks = _split_chunks(axml[header_size:])
    if not chunks or chunks[0][0] != CHUNK_STRING_POOL:
        raise AxmlError("expected a string pool first")

    pool = StringPool(chunks[0][1])

    resource_map_index = None
    for index, (chunk_kind, _chunk) in enumerate(chunks):
        if chunk_kind == CHUNK_RESOURCE_MAP:
            resource_map_index = index
            break
    if resource_map_index is None:
        raise AxmlError("no resource map; this manifest is not one aapt produced")

    resource_ids = list(
        struct.unpack_from(
            f"<{(len(chunks[resource_map_index][1]) - 8) // 4}I", chunks[resource_map_index][1], 8
        )
    )

    # The attribute name has to be the string the resource map ties to
    # android:name, otherwise the platform reads the element as nameless.
    try:
        name_index = resource_ids.index(ATTR_NAME_RESOURCE_ID)
    except ValueError as error:
        raise AxmlError("manifest does not use android:name anywhere") from error

    namespace_index = pool.index_of(ANDROID_NAMESPACE)
    if namespace_index < 0:
        raise AxmlError("android namespace missing from the string pool")

    element_index = pool.index_of("uses-permission")
    if element_index < 0:
        element_index = pool.add("uses-permission")

    permission_index = pool.add(permission)

    start = bytearray()
    start += struct.pack("<HHI", CHUNK_START_ELEMENT, 16, 56)
    start += struct.pack("<II", 0xFFFFFFFF, 0xFFFFFFFF)  # line number, comment
    start += struct.pack("<II", 0xFFFFFFFF, element_index)  # namespace, name
    start += struct.pack("<HHHHHH", 20, 20, 1, 0, 0, 0)
    start += struct.pack("<III", namespace_index, name_index, permission_index)
    start += struct.pack("<HBBI", 8, 0, TYPE_STRING, permission_index)

    end = bytearray()
    end += struct.pack("<HHI", CHUNK_END_ELEMENT, 16, 24)
    end += struct.pack("<II", 0xFFFFFFFF, 0xFFFFFFFF)
    end += struct.pack("<II", 0xFFFFFFFF, element_index)

    rebuilt = bytearray()
    inserted = False
    for chunk_kind, chunk in chunks:
        if chunk_kind == CHUNK_STRING_POOL:
            rebuilt += pool.build()
            continue

        rebuilt += chunk

        # Straight after the opening <manifest>, where a uses-permission is
        # always valid.
        if not inserted and chunk_kind == CHUNK_START_ELEMENT:
            rebuilt += start
            rebuilt += end
            inserted = True

    if not inserted:
        raise AxmlError("no element to attach the permission to")

    return struct.pack("<HHI", CHUNK_XML, header_size, header_size + len(rebuilt)) + bytes(rebuilt)
