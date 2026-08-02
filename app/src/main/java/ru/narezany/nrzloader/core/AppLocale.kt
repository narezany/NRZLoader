package ru.narezany.nrzloader.core

import android.content.Context
import android.content.res.Configuration
import java.util.Locale

/**
 * The language the launcher is shown in.
 *
 * The system language is the default, but a phone sold in one country is often
 * used by someone who reads another, so the choice is kept here and applied to
 * every screen the launcher opens.
 */
object AppLocale {
    /** Follow whatever the phone is set to. */
    const val SYSTEM = "system"

    /** Tags with a translation in res/values-<code>, in the order shown. */
    val supported = listOf(SYSTEM, "en", "ru", "zh")

    private const val PREFS = "nrzloader"
    private const val KEY = "language"

    fun current(context: Context): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, SYSTEM) ?: SYSTEM

    fun set(context: Context, tag: String) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY, tag)
            .apply()
    }

    /**
     * The context an activity should actually use.
     *
     * Doing it this way rather than through the support library keeps the
     * behaviour identical on every Android version the launcher runs on, and
     * the launcher is small enough that carrying a whole compatibility library
     * for one setting is not worth it.
     */
    fun wrap(base: Context): Context {
        val tag = current(base)
        if (tag == SYSTEM) return base

        val locale = Locale.forLanguageTag(tag)
        Locale.setDefault(locale)

        val config = Configuration(base.resources.configuration)
        config.setLocale(locale)
        config.setLayoutDirection(locale)
        return base.createConfigurationContext(config)
    }

    /** The name of a language, written in that language. */
    fun displayName(tag: String): String = when (tag) {
        SYSTEM -> ""
        else -> Locale.forLanguageTag(tag).let { it.getDisplayLanguage(it) }
            .replaceFirstChar { it.titlecase(Locale.ROOT) }
    }
}
