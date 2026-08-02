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
            if (name in wanted) found.add(name)
            if (samples.size < 40) samples.add(name)
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
                nameAt(buffer, at, filled)?.let { tally.add(it, wanted) }
                at++
            }

            for ((text, pattern) in patterns) {
                tally.anchors[text] = tally.anchors.getValue(text) +
                    count(buffer, filled, limit, pattern)
            }

            carried = filled - limit
            if (carried > 0) System.arraycopy(buffer, limit, buffer, 0, carried)
        }
        return tally
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
