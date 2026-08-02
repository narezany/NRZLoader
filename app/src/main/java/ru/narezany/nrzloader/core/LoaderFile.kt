package ru.narezany.nrzloader.core

import android.content.Context
import java.io.File

/**
 * Копия загрузчика рядом с модами.
 *
 * Внутри пересобранной игры лежит та версия загрузчика, что была на момент
 * сборки. Пересобирать полугигабайтный пакет ради правки в мегабайтной
 * библиотеке — долго и обидно, поэтому лаунчер кладёт свежую копию в свою
 * папку, а игра при запуске предпочитает её.
 *
 * Значит, обновление лаунчера обновляет и загрузчик: игре хватает
 * перезапуска. Пересборка нужна только когда меняется то, что вшивается в
 * сам пакет, — разрешения, классы, точка входа.
 */
object LoaderFile {
    private const val ASSET = "libnrzloader.so"

    val path: File get() = File(ModsFolder.root, ASSET)

    /**
     * Обновляет копию, если она отличается от той, что в лаунчере.
     *
     * Сравнение по размеру: собранная заново библиотека почти наверняка
     * другого размера, а совпадение размера при разном содержимом означало
     * бы, что и менять было нечего.
     */
    fun refresh(context: Context): Boolean = runCatching {
        val fresh = context.assets.open(ASSET).use { it.readBytes() }
        if (fresh.isEmpty()) return false

        val target = path
        if (target.isFile() && target.length() == fresh.size.toLong()) return false

        target.parentFile?.mkdirs()
        target.writeBytes(fresh)
        true
    }.getOrDefault(false)
}
