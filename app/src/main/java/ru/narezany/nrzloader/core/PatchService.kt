package ru.narezany.nrzloader.core

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Runs the patch as a foreground service.
 *
 * Repacking several hundred megabytes takes minutes, and nobody wants to stare
 * at the screen for them. A foreground service with a notification is what
 * keeps Android from killing the work once the app is no longer in front.
 */
class PatchService : Service() {
    private val scope = CoroutineScope(SupervisorJob())
    private var lastPercent = -1

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val source = intent?.getStringExtra(EXTRA_SOURCE)
        val label = intent?.getStringExtra(EXTRA_LABEL) ?: "NRZLoader"

        if (source == null) {
            stopSelf(startId)
            return START_NOT_STICKY
        }

        createChannel()
        startForegroundCompat(notification("Подготовка", 0))

        scope.launch {
            PatchState.running("Подготовка", 0)

            val outcome = PatchRunner.run(applicationContext, source, label) { message, percent ->
                PatchState.running(message, percent)
                // The system throttles notification updates, so only whole
                // percent changes are pushed.
                if (percent != lastPercent) {
                    lastPercent = percent
                    notify(notification(message, percent))
                }
            }

            when (outcome) {
                is PatchRunner.Outcome.Success -> {
                    PatchState.done(outcome.apk.absolutePath)
                    notifyFinal("Готово", "Пакет собран, можно устанавливать")
                }
                is PatchRunner.Outcome.Failure -> {
                    PatchState.failed(outcome.message)
                    notifyFinal("Не вышло", outcome.message.take(120))
                }
            }

            stopForegroundCompat()
            stopSelf(startId)
        }

        return START_NOT_STICKY
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    // ------------------------------------------------------------------
    // Notifications
    // ------------------------------------------------------------------

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL) != null) return

        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, "Сборка", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Ход сборки моднутой версии"
                setShowBadge(false)
            }
        )
    }

    private fun contentIntent(): PendingIntent {
        val intent = packageManager.getLaunchIntentForPackage(packageName)
        return PendingIntent.getActivity(
            this, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    private fun notification(message: String, percent: Int): Notification =
        NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle("Собираем моднутую версию")
            .setContentText(message)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setProgress(100, percent, false)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(contentIntent())
            .build()

    private fun notifyFinal(title: String, text: String) {
        val done = NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download_done)
            .setAutoCancel(true)
            .setContentIntent(contentIntent())
            .build()
        runCatching {
            androidx.core.app.NotificationManagerCompat.from(this).notify(DONE_ID, done)
        }
    }

    private fun notify(notification: Notification) {
        runCatching {
            androidx.core.app.NotificationManagerCompat.from(this).notify(ONGOING_ID, notification)
        }
    }

    private fun startForegroundCompat(notification: Notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(ONGOING_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(ONGOING_ID, notification)
        }
    }

    @Suppress("DEPRECATION")
    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            stopForeground(true)
        }
    }

    companion object {
        private const val CHANNEL = "patch"
        private const val ONGOING_ID = 1
        private const val DONE_ID = 2

        private const val EXTRA_SOURCE = "source"
        private const val EXTRA_LABEL = "label"

        fun start(context: Context, sourceApk: String, label: String) {
            val intent = Intent(context, PatchService::class.java).apply {
                putExtra(EXTRA_SOURCE, sourceApk)
                putExtra(EXTRA_LABEL, label)
            }
            context.startForegroundService(intent)
        }
    }
}
