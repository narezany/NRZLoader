package ru.narezany.nrzloader.core

import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.ByteArrayInputStream

/**
 * Разбор списка перемещений — то, без чего на настоящей игре всё встало:
 * в данных девять десятых нулей, а адреса лежат отдельно.
 *
 * Упакованный формат проверяется прогоном через собственную упаковку: она
 * написана по описанию формата и намеренно пользуется всеми его сокращениями,
 * так что разбор проходит все ветки, а не только простую.
 */
class RelocationsTest {
    private val dataAddress = 0x5000L

    private fun target(size: Int = 128) = Relocations.Target(dataAddress, ByteArray(size))

    private fun read(target: Relocations.Target, at: Long): Long {
        var value = 0L
        val offset = (at - target.address).toInt()
        for (index in 7 downTo 0) {
            value = (value shl 8) or (target.bytes[offset + index].toLong() and 0xFF)
        }
        return value
    }

    /** Библиотека с одной секцией данных и списком перемещений внутри. */
    private fun library(relocationBody: ByteArray, type: Int): ByteArray {
        val builder = ElfBuilder()
        builder.section(".data.rel.ro", 1, dataAddress, ByteArray(128))
        builder.section(".rela.dyn", type, 0x9000, relocationBody)
        return builder.build()
    }

    private fun apply(relocationBody: ByteArray, type: Int, into: Relocations.Target):
        Relocations.Outcome {
        val file = library(relocationBody, type)
        val sections = ElfLayout.read { ByteArrayInputStream(file) }
        return Relocations.apply({ ByteArrayInputStream(file) }, sections, listOf(into))
    }

    // ------------------------------------------------------------------
    // Обычный формат
    // ------------------------------------------------------------------

    @Test
    fun `plain entries put their value in place`() {
        val body = ByteArray(48)
        putLong(body, 0, dataAddress + 16)
        putLong(body, 8, 1027)            // относительное перемещение
        putLong(body, 16, 0xABCDEF00L)
        putLong(body, 24, dataAddress + 32)
        putLong(body, 32, 1027)
        putLong(body, 40, 0x11223344L)

        val into = target()
        val outcome = apply(body, 4, into)

        assertEquals("RELA", outcome.kind)
        assertEquals(2, outcome.applied)
        assertEquals(0xABCDEF00L, read(into, dataAddress + 16))
        assertEquals(0x11223344L, read(into, dataAddress + 32))
    }

    @Test
    fun `entries of another kind are left alone`() {
        val body = ByteArray(24)
        putLong(body, 0, dataAddress + 16)
        putLong(body, 8, 1024)            // не относительное
        putLong(body, 16, 0xABCDEF00L)

        val into = target()
        assertEquals(0, apply(body, 4, into).applied)
        assertEquals(0L, read(into, dataAddress + 16))
    }

    @Test
    fun `entries pointing outside the data are skipped`() {
        val body = ByteArray(24)
        putLong(body, 0, 0x99000L)        // мимо секции
        putLong(body, 8, 1027)
        putLong(body, 16, 0xABCDEF00L)

        assertEquals(0, apply(body, 4, target()).applied)
    }

    // ------------------------------------------------------------------
    // Упакованный формат
    // ------------------------------------------------------------------

    /** Упаковщик по описанию формата: одна группа, всё вынесено за скобки. */
    private class Packer {
        private val out = ArrayList<Byte>()

        init {
            "APS2".forEach { out.add(it.code.toByte()) }
        }

        fun value(number: Long): Packer {
            var left = number
            var more = true
            while (more) {
                var byte = (left and 0x7F).toInt()
                left = left shr 7
                val signBit = byte and 0x40 != 0
                more = !((left == 0L && !signBit) || (left == -1L && signBit))
                if (more) byte = byte or 0x80
                out.add(byte.toByte())
            }
            return this
        }

        fun bytes(): ByteArray = out.toByteArray()
    }

    @Test
    fun `a packed group with a shared step and value`() {
        // Три перемещения подряд, через одинаковый шаг, с одним значением:
        // формат позволяет записать это один раз, и разбор обязан развернуть.
        val body = Packer()
            .value(3)                    // всего перемещений
            .value(dataAddress)          // начальное место
            .value(3)                    // размер группы
            .value(2L or 1L or 8L or 4L) // шаг, вид, значение — общие
            .value(8)                    // шаг
            .value(1027)                 // вид
            .value(0x1000)               // значение
            .bytes()

        val into = target()
        val outcome = apply(body, 0x60000001, into)

        assertEquals("APS2", outcome.kind)
        assertEquals(3, outcome.applied)
        // Место сдвигается на шаг перед каждой записью, значение копится.
        assertEquals(0x1000L, read(into, dataAddress + 8))
        assertEquals(0x2000L, read(into, dataAddress + 16))
        assertEquals(0x3000L, read(into, dataAddress + 24))
    }

    @Test
    fun `a packed group that spells everything out`() {
        val body = Packer()
            .value(2)
            .value(dataAddress)
            .value(2)      // размер группы
            .value(8L)     // значения есть, но ничего не вынесено за скобки
            .value(16).value(1027).value(0x4444)
            .value(8).value(1027).value(0x1111)
            .bytes()

        val into = target()
        val outcome = apply(body, 0x60000001, into)

        assertEquals(2, outcome.applied)
        assertEquals(0x4444L, read(into, dataAddress + 16))
        assertEquals(0x5555L, read(into, dataAddress + 24))
    }

    @Test
    fun `relr needs no work because the values are already there`() {
        val builder = ElfBuilder()
        builder.section(".data.rel.ro", 1, dataAddress, ByteArray(128))
        builder.section(".relr.dyn", 19, 0x9000, ByteArray(16))
        val file = builder.build()

        val sections = ElfLayout.read { ByteArrayInputStream(file) }
        val outcome = Relocations.apply({ ByteArrayInputStream(file) }, sections, listOf(target()))

        assertEquals("relr", outcome.kind)
    }

    private fun putLong(data: ByteArray, at: Int, value: Long) {
        for (index in 0 until 8) {
            data[at + index] = ((value shr (index * 8)) and 0xFF).toByte()
        }
    }
}
