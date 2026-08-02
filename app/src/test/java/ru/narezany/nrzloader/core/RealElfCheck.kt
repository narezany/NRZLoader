package ru.narezany.nrzloader.core

import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

class RealElfCheck {
    @Test
    fun check() {
        val path = System.getProperty("nrz.so") ?: return
        val file = File(path)
        assumeTrue(file.isFile)

        val sections = ElfLayout.read { file.inputStream() }
        println("секций: ${sections.size}")
        sections.filter { it.size > 0 }.take(40).forEach {
            println("  %-22s тип=%d смещение=0x%x размер=%d".format(it.name, it.type, it.offset, it.size))
        }
        val data = ElfLayout.dataSections(sections)
        println("данные: " + data.joinToString { it.name })

        val tally = TypeNameScan.scanRegions(
            file.inputStream(), data.map { it.offset to it.size },
            setOf("Actor", "Mob", "Player", "Level", "BlockLegacy", "ItemStack"), emptyList())
        println("имён в данных: ${tally.names}, найдено: ${tally.found}")
        println("примеры: ${tally.samples.take(15)}")
    }
}
