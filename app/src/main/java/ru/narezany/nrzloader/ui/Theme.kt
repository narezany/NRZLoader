package ru.narezany.nrzloader.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

// A green in the spirit of the game, used only where the system cannot supply
// its own palette.
private val Seed = Color(0xFF4C8C3F)

private val DarkColors = darkColorScheme(
    primary = Color(0xFF9ED18C),
    onPrimary = Color(0xFF10380A),
    primaryContainer = Color(0xFF275019),
    onPrimaryContainer = Color(0xFFB9EEA6),
    secondary = Color(0xFFBBCBB1),
    tertiary = Color(0xFFA0CFD3),
)

private val LightColors = lightColorScheme(
    primary = Seed,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFCDEDBB),
    onPrimaryContainer = Color(0xFF0A2005),
    secondary = Color(0xFF54624D),
    tertiary = Color(0xFF386668),
)

@Composable
fun NrzTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current

    // Material You where the platform offers it, a fixed palette otherwise.
    val colors = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> DarkColors
        else -> LightColors
    }

    MaterialTheme(colorScheme = colors, content = content)
}
