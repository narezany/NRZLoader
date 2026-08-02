package ru.narezany.nrzloader.core

import java.io.InputStream

/**
 * Где что лежит внутри библиотеки.
 *
 * Нужно ровно затем, чтобы искать имена классов только там, где они могут
 * быть — в данных. Библиотека игры на две трети состоит из машинного кода, и
 * поиск по нему даёт тысячи ложных совпадений: короткая последовательность
 * «цифра, пара букв, ноль» встречается в командах процессора сплошь и рядом.
 *
 * Заголовки секций лежат в конце файла, поэтому до них приходится проматывать
 * весь файл. Это дешевле, чем читать его в память: трёхсот мегабайт
 * приложению никто не даст.
 */
object ElfLayout {
    data class Section(
        val name: String,
        val type: Int,
        val address: Long,
        val offset: Long,
        val size: Long,
    ) {
        val end: Long get() = offset + size
    }

    /** Секции, в которых вообще могут лежать строки с именами классов. */
    private val DATA_SECTIONS = setOf(
        ".rodata", ".data.rel.ro", ".data.rel.ro.local", ".rdata", ".data",
    )

    class NotAnElf(message: String) : IllegalStateException(message)

    /**
     * @param open открывает файл сначала; вызывается больше одного раза,
     *   потому что по потоку назад не отмотать
     */
    fun read(open: () -> InputStream): List<Section> {
        val header = ByteArray(64)
        open().use { it.readFully(header, 0, 64) }

        if (!(header[0] == 0x7F.toByte() && header[1] == 'E'.code.toByte() &&
                header[2] == 'L'.code.toByte() && header[3] == 'F'.code.toByte())) {
            throw NotAnElf("это не ELF")
        }
        if (header[4] != 2.toByte()) throw NotAnElf("не 64-битный ELF")

        val tableAt = readLong(header, 0x28)
        val entrySize = readShort(header, 0x3A)
        val count = readShort(header, 0x3C)
        val namesIndex = readShort(header, 0x3E)

        if (tableAt <= 0 || count <= 0 || entrySize < 64) throw NotAnElf("нет таблицы секций")

        val raw = ByteArray(count * entrySize)
        val namesRegion: Pair<Long, Long>

        open().use { stream ->
            stream.skipFully(tableAt)
            stream.readFully(raw, 0, raw.size)

            val base = namesIndex * entrySize
            namesRegion = readLong(raw, base + 0x18 + 8) to readLong(raw, base + 0x20 + 8)
        }

        // Таблица имён обычно лежит рядом с заголовками секций, но с какой
        // стороны — не гарантировано, поэтому файл открывается ещё раз.
        val nameBytes = ByteArray(namesRegion.second.toInt().coerceAtMost(1 shl 20))
        open().use { stream ->
            stream.skipFully(namesRegion.first)
            stream.readFully(nameBytes, 0, nameBytes.size)
        }

        return (0 until count).map { index ->
            val at = index * entrySize
            val nameOffset = readInt(raw, at)
            Section(
                name = readCString(nameBytes, nameOffset),
                type = readInt(raw, at + 4),
                address = readLong(raw, at + 0x10),
                offset = readLong(raw, at + 0x18),
                size = readLong(raw, at + 0x20),
            )
        }
    }

    /** Секции с данными, по возрастанию смещения — в таком порядке их и читать. */
    fun dataSections(all: List<Section>): List<Section> =
        all.filter { it.name in DATA_SECTIONS && it.size > 0 && it.type == 1 /* PROGBITS */ }
            .sortedBy { it.offset }

    // ------------------------------------------------------------------
    // Мелочи
    // ------------------------------------------------------------------

    private fun readShort(data: ByteArray, at: Int): Int =
        (data[at].toInt() and 0xFF) or ((data[at + 1].toInt() and 0xFF) shl 8)

    private fun readInt(data: ByteArray, at: Int): Int =
        (data[at].toInt() and 0xFF) or
            ((data[at + 1].toInt() and 0xFF) shl 8) or
            ((data[at + 2].toInt() and 0xFF) shl 16) or
            ((data[at + 3].toInt() and 0xFF) shl 24)

    private fun readLong(data: ByteArray, at: Int): Long {
        var value = 0L
        for (index in 7 downTo 0) {
            value = (value shl 8) or (data[at + index].toLong() and 0xFF)
        }
        return value
    }

    private fun readCString(data: ByteArray, at: Int): String {
        if (at < 0 || at >= data.size) return ""
        var end = at
        while (end < data.size && data[end] != 0.toByte()) end++
        return String(data, at, end - at, Charsets.US_ASCII)
    }
}

/** Читает ровно столько, сколько просили, или падает. */
internal fun InputStream.readFully(into: ByteArray, at: Int, count: Int) {
    var done = 0
    while (done < count) {
        val step = read(into, at + done, count - done)
        if (step < 0) throw IllegalStateException("файл кончился раньше времени")
        done += step
    }
}

/**
 * Промотка вперёд.
 *
 * skip() имеет право пропустить меньше, чем просили, и на потоке из zip это
 * происходит постоянно, поэтому одного вызова недостаточно.
 */
internal fun InputStream.skipFully(count: Long) {
    var left = count
    val scratch = ByteArray(1 shl 16)
    while (left > 0) {
        val skipped = skip(left)
        if (skipped > 0) {
            left -= skipped
            continue
        }
        val step = read(scratch, 0, minOf(scratch.size.toLong(), left).toInt())
        if (step < 0) throw IllegalStateException("файл кончился раньше времени")
        left -= step
    }
}
