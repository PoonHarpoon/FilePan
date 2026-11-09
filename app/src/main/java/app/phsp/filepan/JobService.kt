// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder

/**
 * Keeps the process in the foreground while native file jobs run, with a progress notification.
 * The jobs themselves run on native threads; this only exists so they aren't killed with the
 * activity in the background. Native code starts it when jobs become active and stops it when
 * they're done.
 */
class JobService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        running = true
        startForeground(NOTIFICATION_ID, build(this), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        // The jobs finished before the service got here: stopping a service started with
        // startForegroundService before it calls startForeground isn't allowed.
        if (stopRequested) stopSelf()
        return START_NOT_STICKY
    }

    // dataSync services get a few hours a day on Android 15 and later.
    override fun onTimeout(startId: Int, fgsType: Int) {
        stopSelf()
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL_ID = "jobs"
        private const val NOTIFICATION_ID = 1

        private var running = false
        private var stopRequested = false
        private var title = ""
        private var text = ""
        private var progressPermille = -1

        /** Main thread, like the rest of this companion. */
        fun start(context: Context) {
            createChannel(context)
            stopRequested = false
            context.startForegroundService(Intent(context, JobService::class.java))
        }

        fun stop(context: Context) {
            if (running) {
                context.stopService(Intent(context, JobService::class.java))
            } else {
                stopRequested = true
            }
            title = ""
            text = ""
            progressPermille = -1
        }

        fun update(context: Context, title: String, text: String, progressPermille: Int) {
            this.title = title
            this.text = text
            this.progressPermille = progressPermille
            if (running) {
                context.getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, build(context))
            }
        }

        private fun createChannel(context: Context) {
            val channel = NotificationChannel(
                CHANNEL_ID, context.getString(R.string.jobs_channel), NotificationManager.IMPORTANCE_LOW,
            )
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }

        private fun build(context: Context): Notification {
            val open = PendingIntent.getActivity(
                context, 0,
                Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE,
            )
            return Notification.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.launcher_icon_monochrome)
                .setContentTitle(title.ifEmpty { context.getString(R.string.jobs_channel) })
                .setContentText(text)
                .setProgress(1000, progressPermille.coerceAtLeast(0), progressPermille < 0)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(open)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
                .build()
        }
    }
}
