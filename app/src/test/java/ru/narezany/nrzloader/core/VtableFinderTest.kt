package ru.narezany.nrzloader.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream

/**
 * Цепочка «имя → typeinfo → таблица методов» проверяется на файле, собранном
 * здесь же по правилам C++, где известно, что должно найтись и по каким
 * адресам.
 */
class VtableFinderTest {
    private val textAddress = 0x1000L
    private val textSize = 0x400
    private val rodataAddress = 0x3000L
    private val dataAddress = 0x5000L

    /**
     * Складывает библиотеку из трёх секций.
     *
     * В .rodata кладётся имя, в .data.rel.ro — typeinfo, ссылающаяся на имя, и
     * таблица, ссылающаяся на typeinfo, а за таблицей четыре адреса внутри
     * .text: это и есть виртуальные методы.
     */
    private fun library(
        name: String = "5Actor",
        pointersZeroed: Boolean = false,
    ): ByteArray {
        val rodata = ByteArray(64)
        val nameBytes = name.toByteArray(Charsets.US_ASCII)
        nameBytes.copyInto(rodata, 16)
        val nameAddress = rodataAddress + 16

        // typeinfo: [0] указатель на свою таблицу, [8] указатель на имя.
        // Таблица методов: [0] сдвиг до начала, [8] typeinfo, дальше методы.
        val data = ByteArray(128)
        val typeInfoAt = 0
        val vtableStart = 32
        if (!pointersZeroed) {
            putLong(data, typeInfoAt, 0xDEAD0000L)
            putLong(data, typeInfoAt + 8, nameAddress)
            putLong(data, vtableStart, 0)
            putLong(data, vtableStart + 8, dataAddress + typeInfoAt)
            for (index in 0 until 4) {
                putLong(data, vtableStart + 16 + index * 8, textAddress + 0x40L * (index + 1))
            }
        }

        val builder = ElfBuilder()
        builder.section(".text", 1, textAddress, ByteArray(textSize) { 0x1F })
        builder.section(".rodata", 1, rodataAddress, rodata)
        builder.section(".data.rel.ro", 1, dataAddress, data)
        return builder.build()
    }

    private fun open(data: ByteArray): () -> ByteArrayInputStream =
        { ByteArrayInputStream(data) }

    @Test
    fun `walks from the name to the table of methods`() {
        val report = VtableFinder.run(open(library()), setOf("Actor"))

        assertEquals(1, report.found.size)
        val found = report.found.single()

        assertEquals("Actor", found.name)
        assertEquals(rodataAddress + 16, found.nameAddress)
        assertEquals(dataAddress, found.typeInfoAddress)
        // Таблица начинается сразу за полем typeinfo.
        assertEquals(dataAddress + 32 + 16, found.vtableAddress)
    }

    @Test
    fun `counts how many methods the table holds`() {
        val report = VtableFinder.run(open(library()), setOf("Actor"))
        assertEquals(4, report.found.single().methodSlots)
    }

    @Test
    fun `finds a class that lives in a namespace`() {
        val report = VtableFinder.run(open(library("N3mce5ActorE")), setOf("Actor"))

        assertEquals(1, report.found.size)
        assertEquals("Actor", report.found.single().name)
    }

    @Test
    fun `says plainly when the addresses are not in the file`() {
        // Ровно тот случай, из-за которого всё может встать: указатели
        // подставляются при загрузке, а в файле нули.
        val report = VtableFinder.run(open(library(pointersZeroed = true)), setOf("Actor"))

        assertTrue(report.found.isEmpty())
        assertTrue(report.pointersRead > 0)
        assertTrue(!report.pointersAreInTheFile)
        assertTrue(report.note.contains("нули"))
    }

    @Test
    fun `reports a name it could not find at all`() {
        val report = VtableFinder.run(open(library()), setOf("Zombie"))

        assertTrue(report.namesLocated.isEmpty())
        assertTrue(report.found.isEmpty())
    }

    private fun putLong(data: ByteArray, at: Int, value: Long) {
        for (index in 0 until 8) {
            data[at + index] = ((value shr (index * 8)) and 0xFF).toByte()
        }
    }
}
