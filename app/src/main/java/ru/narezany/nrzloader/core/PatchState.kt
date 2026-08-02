package ru.narezany.nrzloader.core

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Shared progress of the patch.
 *
 * The work runs in a service so it survives leaving the app, which means the
 * screen cannot own the state. Both sides read and write it here instead.
 */
object PatchState {
    sealed interface Stage {
        data object Idle : Stage
        data class Running(val message: String, val percent: Int) : Stage
        data class Done(val apkPath: String) : Stage
        data class Failed(val message: String) : Stage
    }

    private val mutable = MutableStateFlow<Stage>(Stage.Idle)
    val state: StateFlow<Stage> = mutable

    fun running(message: String, percent: Int) {
        mutable.value = Stage.Running(message, percent)
    }

    fun done(apkPath: String) {
        mutable.value = Stage.Done(apkPath)
    }

    fun failed(message: String) {
        mutable.value = Stage.Failed(message)
    }

    fun reset() {
        mutable.value = Stage.Idle
    }

    val isRunning: Boolean get() = mutable.value is Stage.Running
}
