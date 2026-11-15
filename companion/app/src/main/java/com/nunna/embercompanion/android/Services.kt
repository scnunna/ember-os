package com.nunna.embercompanion.android

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothManager
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentSender
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.nunna.embercompanion.MainActivity
import com.nunna.embercompanion.R
import com.nunna.embercompanion.link.LinkPhase
import com.nunna.embercompanion.protocol.Message
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import java.util.regex.Pattern

/** Keeps the Bluetooth link to the watch alive in the background. */
class LinkService : Service() {
    companion object {
        const val CHANNEL = "link"
        const val ACTION_STOP_RING = "com.nunna.embercompanion.STOP_RING"
        const val ACTION_DISCONNECT = "com.nunna.embercompanion.DISCONNECT"

        fun hasBtPermission(ctx: Context) =
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

        const val EXTRA_USER = "user"

        /** [userInitiated] = the user tapped Pair or Connect, so a new bond may be created. Background starts never pair. */
        fun start(ctx: Context, userInitiated: Boolean = false) {
            AppGraph.init(ctx)
            if (AppGraph.prefs.watchAddress == null || !hasBtPermission(ctx)) return
            ContextCompat.startForegroundService(ctx, Intent(ctx, LinkService::class.java).putExtra(EXTRA_USER, userInitiated))
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, LinkService::class.java).setAction(ACTION_DISCONNECT))
        }
    }

    private var job: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        AppGraph.init(this)
        when (intent?.action) {
            ACTION_STOP_RING -> { AppGraph.stopRinging(); update(); return START_STICKY }
            ACTION_DISCONNECT -> {
                AppGraph.gatt.stop(); job?.cancel()
                stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(); return START_NOT_STICKY
            }
        }
        createChannel()
        val n = build("Connecting to your watch")
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE) else startForeground(1, n)
        val addr = AppGraph.prefs.watchAddress
        if (addr == null || !hasBtPermission(this)) { stopSelf(); return START_NOT_STICKY }
        AppGraph.gatt.start(addr, intent?.getBooleanExtra(EXTRA_USER, false) == true)
        job?.cancel()
        job = AppGraph.scope.launch {
            launch { combine(AppGraph.link.phase, AppGraph.link.watch, AppGraph.ringing) { _, _, _ -> }.collect { update() } }
            launch { while (true) { delay(5_000); runCatching { AppGraph.amber.tick() } } }
            while (true) { delay(60_000); AppGraph.link.tick(); runCatching { AppGraph.link.lifeTick() } }
        }
        return START_STICKY
    }

    private fun update() {
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).notify(1, build(statusText()))
    }

    private fun statusText(): String {
        if (AppGraph.ringing.value) return "Your phone is ringing. Tap Stop."
        val w = AppGraph.link.watch.value
        return when (AppGraph.link.phase.value) {
            LinkPhase.CONNECTED -> "Watch connected" + (w?.let { " - battery ${it.batt}%" } ?: "")
            LinkPhase.CONNECTING -> "Connecting to your watch"
            LinkPhase.DISCONNECTED -> "Watch not in range. Reconnecting"
        }
    }

    private fun build(text: String): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val b = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle("Ember Companion")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
        if (AppGraph.ringing.value) {
            val stop = PendingIntent.getService(this, 1, Intent(this, LinkService::class.java).setAction(ACTION_STOP_RING), PendingIntent.FLAG_IMMUTABLE)
            b.addAction(0, "Stop", stop)
        }
        return b.build()
    }

    private fun createChannel() {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Watch connection", NotificationManager.IMPORTANCE_LOW))
    }

    override fun onDestroy() {
        job?.cancel()
        super.onDestroy()
    }
}

/** Starts the link again after the phone restarts (if a watch was paired). */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED) runCatching { LinkService.start(ctx) }
    }
}

/** Forwards phone notifications to the watch and handles its replies / dismisses. */
class EmberNotificationListener : NotificationListenerService() {
    companion object {
        @Volatile private var instance: EmberNotificationListener? = null
        fun cancel(key: String) { instance?.cancelNotification(key) }

        val KNOWN_MESSAGING = setOf(
            "com.whatsapp", "com.whatsapp.w4b", "org.thoughtcrime.securesms", "org.telegram.messenger",
            "com.google.android.apps.messaging", "com.android.mms", "com.facebook.orca", "com.discord",
            "com.google.android.gm", "com.microsoft.teams", "com.Slack", "com.instagram.android",
        )

        fun accessEnabled(ctx: Context): Boolean =
            Settings.Secure.getString(ctx.contentResolver, "enabled_notification_listeners")?.contains(ctx.packageName) == true

        fun settingsIntent() = Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)

        /** Default rule: messaging apps (category message or a well-known package). Once the user picks apps, only those. */
        fun allowed(prefs: Prefs, pkg: String, category: String?): Boolean =
            if (prefs.notifCustomized) pkg in prefs.notifApps
            else category == Notification.CATEGORY_MESSAGE || pkg in KNOWN_MESSAGING
    }

    override fun onListenerConnected() { instance = this; AppGraph.init(this) }
    override fun onListenerDisconnected() { if (instance === this) instance = null }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        AppGraph.init(this)
        val n = sbn.notification
        if (sbn.packageName == packageName) return
        if (sbn.isOngoing || n.flags and Notification.FLAG_GROUP_SUMMARY != 0) return
        if (!allowed(AppGraph.prefs, sbn.packageName, n.category)) return
        if (!AppGraph.connected()) return
        val extras = n.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        val body = (extras.getCharSequence(Notification.EXTRA_BIG_TEXT) ?: extras.getCharSequence(Notification.EXTRA_TEXT))?.toString() ?: ""
        if (title.isBlank() && body.isBlank()) return
        val label = runCatching {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(sbn.packageName, 0)).toString()
        }.getOrDefault(sbn.packageName)
        val canReply = n.actions?.any { a -> a.remoteInputs?.any { it.allowFreeFormInput } == true } == true
        val id = NotifBridge.register(sbn)
        AppGraph.link.send(Message.Notif(id, label.take(20), title.take(40), body.take(240), if (canReply) NotifBridge.quickReplies() else emptyList()))
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        val id = NotifBridge.idFor(sbn.key) ?: return
        if (AppGraph.connected()) AppGraph.link.send(Message.NotifRm(id))
    }
}

/** Pairing with the Companion Device Manager: shows the system chooser filtered to EmberWatchOS (older firmware: `Watch-XXXX`). */
object Pairing {
    fun request(ctx: Context, onChooser: (IntentSender) -> Unit, onError: (String) -> Unit) {
        val cdm = ctx.getSystemService(Context.COMPANION_DEVICE_SERVICE) as CompanionDeviceManager
        val bt = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        if (bt == null || !bt.isEnabled) { onError(Errors.BT_OFF); return }
        val filter = BluetoothLeDeviceFilter.Builder().setNamePattern(Pattern.compile("^(EmberWatchOS|Watch-).*")).build()
        val req = AssociationRequest.Builder().addDeviceFilter(filter).setSingleDevice(false).build()
        cdm.associate(req, object : CompanionDeviceManager.Callback() {
            @Deprecated("Android 12/12L") override fun onDeviceFound(chooserLauncher: IntentSender) { onChooser(chooserLauncher) }
            override fun onAssociationPending(intentSender: IntentSender) { onChooser(intentSender) }
            override fun onFailure(error: CharSequence?) { onError(Errors.BT_NO_WATCH) }
        }, null)
    }

    /** After the chooser returns: find the address of the association we just made and remember it. */
    @Suppress("DEPRECATION")
    fun finish(ctx: Context, data: Intent?): Boolean {
        val cdm = ctx.getSystemService(Context.COMPANION_DEVICE_SERVICE) as CompanionDeviceManager
        var addr: String? = null
        var name: String? = null
        val sr = data?.let {
            if (Build.VERSION.SDK_INT >= 33) it.getParcelableExtra(CompanionDeviceManager.EXTRA_DEVICE, android.bluetooth.le.ScanResult::class.java)
            else it.getParcelableExtra<android.bluetooth.le.ScanResult>(CompanionDeviceManager.EXTRA_DEVICE)
        }
        if (sr != null) { addr = sr.device.address; name = runCatching { sr.device.name }.getOrNull() ?: sr.scanRecord?.deviceName }
        if (addr == null) addr = if (Build.VERSION.SDK_INT >= 33) cdm.myAssociations.lastOrNull()?.deviceMacAddress?.toString() else cdm.associations.lastOrNull()
        if (addr == null) return false
        AppGraph.prefs.watchAddress = addr.uppercase()
        AppGraph.prefs.watchName = name
        return true
    }
}
