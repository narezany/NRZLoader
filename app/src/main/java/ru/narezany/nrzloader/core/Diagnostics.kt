package ru.narezany.nrzloader.core

import android.content.Context
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * Проверяет всю цепочку целиком и говорит, где она рвётся.
 *
 * Частей много: доступ к файлам, пересобранная игра, копия загрузчика, список
 * таблиц, строка в настройках. Стоит выпасть одной — и наружу это выглядит
 * одинаково: файл не обновляется. Гадать, какая именно, приходилось перепиской,
 * а проверить их все можно за долю секунды прямо здесь.
 */
object Diagnostics {
    /**
     * Начиная с этой версии игра берёт загрузчик из папки рядом с модами.
     * Собранная более старой обновлений загрузчика попросту не увидит.
     */
    private const val EXTERNAL_LOADER_SINCE = "1.7.0"

    data class Check(
        val title: String,
        val ok: Boolean,
        val detail: String,
        /** Что делать, если не сошлось. */
        val fix: String = "",
    )

    fun run(context: Context): List<Check> {
        val checks = ArrayList<Check>()

        val storage = MainActivity_hasStorageAccess()
        checks += Check(
            title = "Доступ к файлам у лаунчера",
            ok = storage,
            detail = if (storage) "есть" else "нет",
            fix = "Дайте доступ кнопкой на главном экране.",
        )

        val installs = GameLocator.findAll(context)
        val patched = installs.firstOrNull { it.patched }
        checks += Check(
            title = "Пересобранная игра стоит",
            ok = patched != null,
            detail = patched?.let { "${it.packageName} ${it.versionName}" } ?: "не найдена",
            fix = "Соберите моднутую версию и установите её.",
        )

        // Внешний загрузчик умеет только та сборка, куда попал новый
        // загрузочный класс. Старая молча берёт вшитую копию, и обновления
        // лаунчера до неё не доходят — выглядит это ровно как «ничего не
        // происходит».
        val builtBy = patched?.loaderVersion.orEmpty()
        val supportsExternal =
            builtBy.isNotBlank() && LoaderVersion.compare(builtBy, EXTERNAL_LOADER_SINCE) >= 0
        checks += Check(
            title = "Игра берёт загрузчик из папки",
            ok = supportsExternal,
            detail = when {
                patched == null -> "игра не найдена"
                builtBy.isBlank() -> "собрана до того, как это появилось"
                supportsExternal -> "да, собрана загрузчиком $builtBy"
                else -> "нет: собрана загрузчиком $builtBy"
            },
            fix = "Соберите игру заново: обновления загрузчика до неё не доходят.",
        )

        val loader = LoaderFile.path
        val bundled = runCatching {
            context.assets.open("libnrzloader.so").use { it.available().toLong() }
        }.getOrDefault(-1L)
        val sameAsLauncher = loader.isFile && bundled > 0 && loader.length() == bundled
        checks += Check(
            title = "Свежий загрузчик лежит в папке",
            ok = sameAsLauncher,
            detail = when {
                !loader.isFile -> "файла нет"
                loader.length() == bundled -> "${loader.length() / 1024} КБ, как в лаунчере"
                else -> "${loader.length() / 1024} КБ, а в лаунчере ${bundled / 1024} КБ"
            },
            fix = "Откройте лаунчер при выданном доступе к файлам — он положит свежий.",
        )

        val tables = File(ModsFolder.config, "vtables.conf")
        val tableCount = runCatching {
            tables.readLines().count { it.startsWith("vtable.") }
        }.getOrDefault(0)
        checks += Check(
            title = "Список таблиц готов",
            ok = tableCount > 0,
            detail = if (tableCount > 0) "$tableCount классов" else "файла нет",
            fix = "Нажмите «Проверить» выше — этот файл пишет она.",
        )

        val probeClass = ProbeSettings.current()
        val known = probeClass.isNotBlank() && runCatching {
            tables.readLines().any { it.startsWith("vtable.$probeClass ") }
        }.getOrDefault(false)
        checks += Check(
            title = "Класс для разведки выбран",
            ok = probeClass.isNotBlank() && known,
            detail = when {
                probeClass.isBlank() -> "не выбран"
                known -> probeClass
                else -> "$probeClass — такого класса нет в списке таблиц"
            },
            fix = "Выберите класс ниже.",
        )

        checks += freshness(
            "Загрузчик запускался вместе с игрой",
            ModsFolder.log,
            "Запустите игру. Если файла нет и после запуска — загрузчик не поднялся.",
        )
        checks += freshness(
            "Разведка писала отчёт",
            File(File(ModsFolder.root, "reports"), "slots.txt"),
            "Смотрите вкладку «Лог»: загрузчик пишет там, почему разведка не встала.",
        )

        return checks
    }

    /** Насколько давно файл трогали: старый файл значит, что его не переписали. */
    private fun freshness(title: String, file: File, fix: String): Check {
        if (!file.isFile) {
            return Check(title, false, "файла нет", fix)
        }

        val age = System.currentTimeMillis() - file.lastModified()
        val minutes = TimeUnit.MILLISECONDS.toMinutes(age)
        val fresh = minutes < 30

        return Check(
            title = title,
            ok = fresh,
            detail = when {
                minutes < 1 -> "меньше минуты назад"
                minutes < 60 -> "$minutes мин назад"
                else -> "${TimeUnit.MINUTES.toHours(minutes)} ч назад — это старый файл"
            },
            fix = fix,
        )
    }

    // Вынесено отдельно, чтобы проверка не зависела от экранов.
    private fun MainActivity_hasStorageAccess(): Boolean =
        android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.R ||
            android.os.Environment.isExternalStorageManager()
}
