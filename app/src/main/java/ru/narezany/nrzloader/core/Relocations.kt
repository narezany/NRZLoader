package ru.narezany.nrzloader.core

import java.io.InputStream

/**
 * Восстанавливает указатели, которых в файле нет.
 *
 * Библиотека не знает заранее, по какому адресу её положат, поэтому все
 * указатели внутри данных компоновщик заменяет нулями, а рядом кладёт список:
 * «по такому-то месту записать такое-то значение». Подставляет их системный
 * загрузчик при запуске.
 *
 * На настоящей игре в данных нулей девять десятых, так что без этого списка от
 * имени класса к таблице его методов не пройти вовсе.
 *
 * Списков бывает три вида, и все три здесь разбираются:
 *
 *  - обычный RELA — записи по 24 байта, значение лежит в третьем поле;
 *  - упакованный APS2, придуманный в Android, чтобы список не занимал
 *    мегабайты: значения сжаты и записаны группами;
 *  - RELR — в нём значение уже лежит на месте, и трогать ничего не нужно.
 */
object Relocations {
    /** Тот вид перемещения, где значение считается от начала библиотеки. */
    private const val R_AARCH64_RELATIVE = 1027L

    private const val SHT_RELA = 4
    private const val SHT_ANDROID_RELA = 0x60000001
    private const val SHT_RELR = 19
    private const val SHT_ANDROID_RELR = 0x6FFFFF00

    /** Кусок данных, в который нужно вписать восстановленные значения. */
    class Target(val address: Long, val bytes: ByteArray) {
        fun contains(at: Long): Boolean = at >= address && at + 8 <= address + bytes.size

        fun write(at: Long, value: Long) {
            var offset = (at - address).toInt()
            var left = value
            repeat(8) {
                bytes[offset++] = (left and 0xFF).toByte()
                left = left ushr 8
            }
        }
    }

    data class Outcome(val kind: String, val applied: Int, val note: String = "")

    /**
     * Читает список и вписывает значения в переданные куски.
     *
     * @return что нашлось и сколько значений подставлено
     */
    fun apply(
        open: () -> InputStream,
        sections: List<ElfLayout.Section>,
        targets: List<Target>,
    ): Outcome {
        val relocation = sections.firstOrNull {
            it.size > 0 && (it.type == SHT_RELA || it.type == SHT_ANDROID_RELA)
        }

        if (relocation == null) {
            val relr = sections.firstOrNull {
                it.size > 0 && (it.type == SHT_RELR || it.type == SHT_ANDROID_RELR)
            }
            return if (relr != null) {
                Outcome("relr", 0, "значения уже лежат на местах")
            } else {
                Outcome("нет", 0, "списка перемещений в библиотеке нет")
            }
        }

        val body = readSection(open, relocation)

        // У упакованного списка в начале стоит метка. У обычного её нет.
        val packed = body.size >= 4 && body[0] == 'A'.code.toByte() &&
            body[1] == 'P'.code.toByte() && body[2] == 'S'.code.toByte() &&
            body[3] == '2'.code.toByte()

        return if (packed) {
            Outcome("APS2", unpack(body, targets))
        } else {
            Outcome("RELA", plain(body, targets))
        }
    }

    // ------------------------------------------------------------------
    // Разбор
    // ------------------------------------------------------------------

    /** Обычный список: смещение, вид, значение — и так по кругу. */
    private fun plain(body: ByteArray, targets: List<Target>): Int {
        var applied = 0
        var at = 0
        while (at + 24 <= body.size) {
            val where = readLong(body, at)
            val info = readLong(body, at + 8)
            val addend = readLong(body, at + 16)
            at += 24

            if ((info and 0xFFFFFFFFL) != R_AARCH64_RELATIVE) continue
            if (put(targets, where, addend)) applied++
        }
        return applied
    }

    /**
     * Упакованный список.
     *
     * Записи идут группами, и внутри группы то, что у всех одинаковое,
     * записано один раз. Что именно вынесено за скобки, говорят флаги группы.
     */
    private fun unpack(body: ByteArray, targets: List<Target>): Int {
        val reader = Sleb(body, 4)  // первые четыре байта — метка

        val total = reader.next()
        var where = reader.next()
        var addend = 0L
        var applied = 0
        var done = 0L

        while (done < total && !reader.finished) {
            val groupSize = reader.next()
            val flags = reader.next()

            val sameStep = flags and GROUPED_BY_OFFSET_DELTA != 0L
            val sameInfo = flags and GROUPED_BY_INFO != 0L
            val hasAddend = flags and HAS_ADDEND != 0L
            val sameAddend = flags and GROUPED_BY_ADDEND != 0L

            val step = if (sameStep) reader.next() else 0L
            val info = if (sameInfo) reader.next() else 0L
            val addendStep = if (hasAddend && sameAddend) reader.next() else 0L

            // Проверять здесь, кончились ли байты, нельзя: когда группа
            // вынесла за скобки всё, на сами записи байтов и не остаётся —
            // а записи всё равно есть, ровно groupSize штук.
            var index = 0L
            while (index < groupSize) {
                where += if (sameStep) step else reader.next()
                val thisInfo = if (sameInfo) info else reader.next()

                if (hasAddend) {
                    addend += if (sameAddend) addendStep else reader.next()
                } else {
                    addend = 0
                }

                if ((thisInfo and 0xFFFFFFFFL) == R_AARCH64_RELATIVE) {
                    if (put(targets, where, addend)) applied++
                }
                index++
                done++
            }
        }
        return applied
    }

    private const val GROUPED_BY_INFO = 1L
    private const val GROUPED_BY_OFFSET_DELTA = 2L
    private const val GROUPED_BY_ADDEND = 4L
    private const val HAS_ADDEND = 8L

    /** Числа переменной длины, которыми записан упакованный список. */
    private class Sleb(private val data: ByteArray, private var at: Int) {
        val finished: Boolean get() = at >= data.size

        fun next(): Long {
            var result = 0L
            var shift = 0
            var byte: Int
            do {
                if (at >= data.size) return result
                byte = data[at++].toInt() and 0xFF
                result = result or ((byte and 0x7F).toLong() shl shift)
                shift += 7
            } while (byte and 0x80 != 0 && shift < 64)

            // Знак: если последний значащий бит стоит, число отрицательное.
            if (shift < 64 && (byte and 0x40) != 0) result = result or (-1L shl shift)
            return result
        }
    }

    private fun put(targets: List<Target>, where: Long, value: Long): Boolean {
        val target = targets.firstOrNull { it.contains(where) } ?: return false
        target.write(where, value)
        return true
    }

    private fun readSection(open: () -> InputStream, section: ElfLayout.Section): ByteArray {
        val body = ByteArray(section.size.coerceAtMost(64L * 1024 * 1024).toInt())
        open().use { stream ->
            stream.skipFully(section.offset)
            stream.readFully(body, 0, body.size)
        }
        return body
    }

    private fun readLong(data: ByteArray, at: Int): Long {
        var value = 0L
        for (index in 7 downTo 0) {
            value = (value shl 8) or (data[at + index].toLong() and 0xFF)
        }
        return value
    }
}
