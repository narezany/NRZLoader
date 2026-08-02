package ru.narezany.nrzloader.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner

/**
 * A counter that ticks whenever the screen comes back to the front.
 *
 * Everything this app shows lives outside it: which packages are installed,
 * what sits in the mods folder, what the loader wrote to its log. Reading that
 * once and holding on to it leaves the screen describing a world that has
 * since moved on, which is exactly what happens after installing a package or
 * dropping in a mod.
 */
@Composable
fun rememberResumeTicker(): State<Int> {
    val owner = LocalLifecycleOwner.current
    val ticks = remember { mutableIntStateOf(0) }

    DisposableEffect(owner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) ticks.intValue++
        }
        owner.lifecycle.addObserver(observer)
        onDispose { owner.lifecycle.removeObserver(observer) }
    }

    return ticks
}
