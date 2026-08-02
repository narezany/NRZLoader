package ru.narezany.nrzloader.core

import java.io.InputStream

/**
 * Ищет в потоке байт искажённые имена классов.
 *
 * В C++ имя класса внутри typeinfo записано как длина числом, потом само имя,
 * потом ноль: `5Actor`. Совпадение длины с объявленной и есть проверка, что
 * перед нами имя, а не просто текст, начинающийся с цифры, — в трёхстах
 * мегабайтах данных такого текста хватает.
 *
 * Ничего от Android здесь нет намеренно: от правильности этого разбора зависит
 * ответ на вопрос, добираются ли моды до геймплея вообще, и проверять такое
 * лучше не на телефоне.
 */
object TypeNameScan {
    private const val CHUNK = 1 shl 20

    /** Длиннее самого длинного имени плюс его длина — на стык окон. */
    const val OVERLAP = 128

    class Tally {
        var names = 0
            private set

        val found = LinkedHashSet<String>()
        val samples = LinkedHashSet<String>()
        val anchors = HashMap<String, Int>()

        internal fun add(name: String, wanted: Set<String>) {
            names++
            // Класс внутри пространства имён — это всё тот же класс, поэтому
            // он засчитывается и по последнему куску имени.
            val short = name.substringAfterLast("::")
            if (name in wanted) found.add(name)
            if (short in wanted) found.add(short)
            // В примеры идут только имена, по которым видно, что это имя.
            // Двухбуквенные совпадения — как раз то, что сыплется само собой,
            // и в отчёте они только сбивают с толку.
            if (samples.size < 40 && (name.length >= 5 || "::" in name)) samples.add(name)
        }
    }

    /**
     * Разбирает поток окнами, а не целиком: библиотека игры на треть
     * гигабайта, и приложению памяти под неё не дадут.
     */
    fun scan(stream: InputStream, wanted: Set<String>, anchors: List<String>): Tally {
        val tally = Tally()
        val patterns = anchors.associateWith { it.toByteArray(Charsets.US_ASCII) }
        anchors.forEach { tally.anchors[it] = 0 }

        scanInto(stream, tally, wanted, patterns)
        return tally
    }

    private fun scanInto(
        stream: InputStream,
        tally: Tally,
        wanted: Set<String>,
        patterns: Map<String, ByteArray>,
    ) {
        val buffer = ByteArray(CHUNK + OVERLAP)
        var carried = 0
        var atEnd = false

        while (!atEnd) {
            val read = stream.read(buffer, carried, CHUNK)
            if (read <= 0) {
                atEnd = true
                if (carried == 0) break
            }

            val filled = carried + maxOf(read, 0)
            // Последнее окно разбирается целиком: за ним ничего нет, и хвост
            // переносить некуда.
            val limit = if (atEnd || filled <= OVERLAP) filled else filled - OVERLAP

            var at = 0
            while (at < limit) {
                val name = nameAt(buffer, at, filled) ?: nestedAt(buffer, at, filled)
                if (name != null) tally.add(name, wanted)
                at++
            }

            for ((text, pattern) in patterns) {
                tally.anchors[text] = tally.anchors.getValue(text) +
                    count(buffer, filled, limit, pattern)
            }

            carried = filled - limit
            if (carried > 0) System.arraycopy(buffer, limit, buffer, 0, carried)
        }
    }

    /**
     * То же самое, но только в перечисленных кусках файла.
     *
     * Куски должны идти по возрастанию смещения: поток проматывается вперёд и
     * назад не отматывается.
     *
     * @param regions пары «смещение, длина»
     */
    fun scanRegions(
        stream: InputStream,
        regions: List<Pair<Long, Long>>,
        wanted: Set<String>,
        anchors: List<String>,
    ): Tally {
        val tally = Tally()
        val patterns = anchors.associateWith { it.toByteArray(Charsets.US_ASCII) }
        anchors.forEach { tally.anchors[it] = 0 }

        var position = 0L
        for ((offset, size) in regions.sortedBy { it.first }) {
            if (offset < position) continue
            stream.skipFully(offset - position)
            position = offset

            scanInto(LimitedInput(stream, size), tally, wanted, patterns)
            position += size
        }
        return tally
    }

    /** Отдаёт ровно `left` байт и делает вид, что дальше файл кончился. */
    private class LimitedInput(private val source: InputStream, private var left: Long) :
        InputStream() {
        override fun read(): Int = throw UnsupportedOperationException()

        override fun read(into: ByteArray, at: Int, count: Int): Int {
            if (left <= 0) return -1
            val step = source.read(into, at, minOf(count.toLong(), left).toInt())
            if (step > 0) left -= step
            return step
        }
    }

    /**
     * Имя внутри пространства имён: `N`, дальше пары «длина, кусок», потом
     * `E`. Так записаны, например, вложенные классы, и без этого их не видно
     * вовсе — а именно так называется добрая половина того, что нужно.
     */
    fun nestedAt(buffer: ByteArray, start: Int, filled: Int): String? {
        if (start > 0 && isNameChar(buffer[start - 1])) return null
        if (start >= filled || buffer[start] != 'N'.code.toByte()) return null

        var at = start + 1
        val parts = ArrayList<String>(4)

        while (at < filled && buffer[at] != 'E'.code.toByte()) {
            var declared = 0
            var digits = 0
            while (at < filled && buffer[at] >= ZERO && buffer[at] <= NINE) {
                declared = declared * 10 + (buffer[at] - ZERO)
                digits++
                at++
                if (digits > 3) return null
            }
            if (digits == 0 || declared < 1 || declared > 80) return null
            if (at + declared > filled) return null

            for (index in at until at + declared) {
                if (!isNameChar(buffer[index])) return null
            }
            parts.add(String(buffer, at, declared, Charsets.US_ASCII))
            at += declared
            if (parts.size > 8) return null
        }

        if (at >= filled || buffer[at] != 'E'.code.toByte()) return null
        if (at + 1 >= filled || buffer[at + 1] != 0.toByte()) return null
        if (parts.isEmpty()) return null

        return parts.joinToString("::")
    }

    /** Имя, начинающееся ровно в этой позиции, или null. */
    fun nameAt(buffer: ByteArray, start: Int, filled: Int): String? {
        // Перед числом не должно стоять буквы или цифры: иначе это середина
        // чего-то другого, и мы бы посчитали одно имя дважды.
        if (start > 0 && isNameChar(buffer[start - 1])) return null

        var at = start
        var declared = 0
        var digits = 0
        while (at < filled && buffer[at] >= ZERO && buffer[at] <= NINE) {
            declared = declared * 10 + (buffer[at] - ZERO)
            digits++
            at++
            if (digits > 3) return null
        }
        if (digits == 0 || declared < 2 || declared > 80) return null
        if (at + declared >= filled) return null

        if (!isLetter(buffer[at])) return null
        for (index in at until at + declared) {
            if (!isNameChar(buffer[index])) return null
        }
        if (buffer[at + declared] != 0.toByte()) return null

        return String(buffer, at, declared, Charsets.US_ASCII)
    }

    /**
     * Считает вхождения, начинающиеся до `limit`: то, что начинается позже,
     * будет посчитано в следующем окне, куда этот хвост и переезжает.
     */
    private fun count(buffer: ByteArray, filled: Int, limit: Int, pattern: ByteArray): Int {
        var total = 0
        var at = 0
        outer@ while (at < limit && at <= filled - pattern.size) {
            for (index in pattern.indices) {
                if (buffer[at + index] != pattern[index]) {
                    at++
                    continue@outer
                }
            }
            total++
            at += pattern.size
        }
        return total
    }

    private const val ZERO = '0'.code.toByte()
    private const val NINE = '9'.code.toByte()

    private fun isLetter(byte: Byte): Boolean =
        byte in 'A'.code.toByte()..'Z'.code.toByte() ||
            byte in 'a'.code.toByte()..'z'.code.toByte() ||
            byte == '_'.code.toByte()

    private fun isNameChar(byte: Byte): Boolean =
        isLetter(byte) || byte in ZERO..NINE
}
