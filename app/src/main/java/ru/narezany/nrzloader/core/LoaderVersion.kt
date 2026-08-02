package ru.narezany.nrzloader.core

/**
 * The loader's version, as the launcher knows it.
 *
 * A mod can say which loader versions it was written for, so this number has
 * to mean the same thing in three places: here, in the native loader's header,
 * and in the VERSION file at the root of the project. A test compares them
 * rather than trusting anyone to remember.
 */
object LoaderVersion {
    const val VALUE = "1.9.1"

    /**
     * Compares dotted versions, returning a negative number, zero, or a
     * positive one. Missing parts count as zero, so "1.2" equals "1.9.1", and
     * anything that is not a number counts as zero rather than throwing:
     * a mod's manifest is written by hand and will eventually contain junk.
     */
    fun compare(a: String, b: String): Int {
        val left = a.split('.')
        val right = b.split('.')

        for (index in 0 until maxOf(left.size, right.size)) {
            val one = left.getOrNull(index)?.trim()?.toIntOrNull() ?: 0
            val two = right.getOrNull(index)?.trim()?.toIntOrNull() ?: 0
            if (one != two) return one - two
        }
        return 0
    }

    /** Whether this loader is inside the range a mod asks for. */
    fun satisfies(minimum: String, maximum: String): Boolean {
        if (minimum.isNotBlank() && compare(VALUE, minimum) < 0) return false
        if (maximum.isNotBlank() && compare(VALUE, maximum) > 0) return false
        return true
    }
}
