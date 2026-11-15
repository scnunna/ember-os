package com.nunna.embercompanion.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.clickable
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.health.connect.client.PermissionController
import com.nunna.embercompanion.android.*
import com.nunna.embercompanion.link.LinkPhase
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.launch
import java.time.LocalDate

@Composable
fun HomeScreen(go: (Screen) -> Unit) {
    val ctx = LocalContext.current
    val phase by AppGraph.link.phase.collectAsState()
    val watch by AppGraph.link.watch.collectAsState()
    val days by AppGraph.steps.days.collectAsState()
    val ringing by AppGraph.ringing.collectAsState()
    val watchErr by AppGraph.watchError.collectAsState()
    val issue by AppGraph.gatt.issue.collectAsState()
    val bond by AppGraph.gatt.bond.collectAsState()
    var error by remember { mutableStateOf<String?>(null) }
    var paired by remember { mutableStateOf(AppGraph.prefs.watchAddress != null) }

    fun startLink() { LinkService.start(ctx, userInitiated = true) }

    val chooser = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) { r ->
        if (Pairing.finish(ctx, r.data)) { paired = true; error = null; startLink() }
        else if (r.resultCode != android.app.Activity.RESULT_CANCELED) error = Errors.BT_NO_WATCH
    }
    fun doPair() {
        Pairing.request(ctx, { chooser.launch(IntentSenderRequest.Builder(it).build()) }, { error = it })
    }
    val perms = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { res ->
        if (res[Manifest.permission.BLUETOOTH_CONNECT] == false) error = Errors.BT_PERMISSION
        else { error = null; if (paired) startLink() else doPair() }
    }
    fun needed(): Array<String> {
        val l = arrayListOf(Manifest.permission.BLUETOOTH_CONNECT)
        if (Build.VERSION.SDK_INT >= 33) l += Manifest.permission.POST_NOTIFICATIONS
        return l.filter { ContextCompat.checkSelfPermission(ctx, it) != PackageManager.PERMISSION_GRANTED }.toTypedArray()
    }
    fun pairOrConnect() {
        val n = needed()
        if (n.isNotEmpty()) perms.launch(n) else if (paired) startLink() else doPair()
    }

    Page("Ember Companion", null) {
        if (ringing) Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer)) {
            Row(Modifier.padding(16.dp).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Your phone is ringing (the watch asked).", Modifier.weight(1f))
                Button(onClick = { AppGraph.stopRinging() }) { Text("Stop") }
            }
        }
        if (issue == com.nunna.embercompanion.link.LinkIssue.TYPE_CODE) Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer)) {
            Text("Type the 6-digit code shown on your watch into the Bluetooth pairing box that Android is showing.", Modifier.padding(16.dp), style = MaterialTheme.typography.titleMedium)
        }
        ErrorText(error ?: AppGraph.gatt.lastError.takeIf { paired && phase != LinkPhase.CONNECTED && issue != com.nunna.embercompanion.link.LinkIssue.TYPE_CODE })
        if (issue == com.nunna.embercompanion.link.LinkIssue.BOND_LOST || issue == com.nunna.embercompanion.link.LinkIssue.PAIRING_FAILED) {
            OutlinedButton(onClick = { ctx.startActivity(Intent(android.provider.Settings.ACTION_BLUETOOTH_SETTINGS)) }) { Text("Open Bluetooth settings (forget the watch there)") }
        }
        ErrorText(watchErr)
        InfoCard("Your watch") {
            val name = watch?.name ?: AppGraph.prefs.watchName ?: "Not paired yet"
            Text(name, style = MaterialTheme.typography.headlineSmall)
            Text(
                when (phase) {
                    LinkPhase.CONNECTED -> "Connected"
                    LinkPhase.CONNECTING -> "Connecting..."
                    LinkPhase.DISCONNECTED -> if (paired) "Not connected. It reconnects by itself when the watch is near." else "Turn on your watch, then tap Pair."
                },
            )
            watch?.let {
                Text("Battery ${it.batt}%" + if (it.usb) " (charging)" else "")
                Text("Steps today: ${it.steps}")
                Text("Watch software ${it.fw}")
            }
            if (!paired || bond != com.nunna.embercompanion.link.Bond.BONDED) Text("To pair: on the watch open Settings > Pair new phone. The watch shows a 6-digit code. Then tap the button below and type that code on your phone.", style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { pairOrConnect() }) { Text(if (paired) "Connect now" else "Pair my watch") }
                if (paired) {
                    OutlinedButton(onClick = { doPairAgain(ctx) { paired = false; AppGraph.gatt.forgetBond(); AppGraph.gatt.stop(); LinkService.stop(ctx) } }) { Text("Forget") }
                }
            }
            if (phase == LinkPhase.CONNECTED) {
                OutlinedButton(onClick = { AppGraph.link.send(Message.FindWatch) }) { Text("Find my watch (it will buzz)") }
            }
        }
        val items = listOf(
            Triple("Notifications", "Choose which apps buzz your watch.", Screen.NOTIFS),
            Triple("Steps", "Your steps this week, saved to Health Connect.", Screen.STEPS),
            Triple("Watch apps", "Add or remove little apps on the watch.", Screen.APPS),
            Triple("Meeting notes", "Recordings from the watch, turned into notes.", Screen.NOTES),
            Triple("Travel translate", "Talk to someone in another language.", Screen.TRANSLATE),
            Triple("Guitar practice", "Minutes, streak and tempo.", Screen.PRACTICE),
            Triple("Settings", "Speech model, AI summary file, permissions.", Screen.SETTINGS),
        )
        for ((t, d, s) in items) Card(Modifier.fillMaxWidth().clickable { go(s) }) {
            Column(Modifier.padding(16.dp)) { Text(t, style = MaterialTheme.typography.titleMedium); Text(d) }
        }
    }
}

private fun doPairAgain(ctx: android.content.Context, then: () -> Unit) {
    AppGraph.prefs.watchAddress = null
    AppGraph.prefs.watchName = null
    then()
}

// ---------------------------------------------------------------------------- Notifications

private data class AppRow(val pkg: String, val label: String)

@Composable
fun NotifScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    var enabled by remember { mutableStateOf(EmberNotificationListener.accessEnabled(ctx)) }
    val prefs = AppGraph.prefs
    var chosen by remember { mutableStateOf(if (prefs.notifCustomized) prefs.notifApps else emptySet()) }
    var custom by remember { mutableStateOf(prefs.notifCustomized) }
    val apps = remember {
        val pm = ctx.packageManager
        val i = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        pm.queryIntentActivities(i, 0).map { AppRow(it.activityInfo.packageName, it.loadLabel(pm).toString()) }
            .distinctBy { it.pkg }.filter { it.pkg != ctx.packageName }.sortedBy { it.label.lowercase() }
    }
    val lifecycle = androidx.lifecycle.compose.LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle) {
        val ob = androidx.lifecycle.LifecycleEventObserver { _, e -> if (e == androidx.lifecycle.Lifecycle.Event.ON_RESUME) enabled = EmberNotificationListener.accessEnabled(ctx) }
        lifecycle.addObserver(ob); onDispose { lifecycle.removeObserver(ob) }
    }
    Page("Notifications", onBack, scroll = false) {
        if (!enabled) {
            ErrorText(Errors.NOTIF_ACCESS)
            Button(onClick = { ctx.startActivity(EmberNotificationListener.settingsIntent()) }) { Text("Open the setting") }
        } else Text("Notification access is on.")
        Row(verticalAlignment = Alignment.CenterVertically) {
            Switch(checked = custom, onCheckedChange = { custom = it; if (it) prefs.notifApps = chosen else prefs.setNotifCustom(false) })
            Spacer(Modifier.width(8.dp))
            Text(if (custom) "Only the apps ticked below" else "Messaging apps only (default)")
        }
        Text("Quick replies on the watch: ${NotifBridge.quickReplies().joinToString(", ")}. They only show for chats that allow replying.", style = MaterialTheme.typography.bodySmall)
        if (custom) LazyColumn(Modifier.weight(1f)) {
            items(apps) { a ->
                Row(Modifier.fillMaxWidth().clickable {
                    chosen = if (a.pkg in chosen) chosen - a.pkg else chosen + a.pkg
                    prefs.notifApps = chosen
                }, verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = a.pkg in chosen, onCheckedChange = null)
                    Spacer(Modifier.width(8.dp))
                    Text(a.label)
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------- Steps

@Composable
fun StepsScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val days by AppGraph.steps.days.collectAsState()
    val msg by AppGraph.healthMessage.collectAsState()
    val phase by AppGraph.link.phase.collectAsState()
    val launcher = rememberLauncherForActivityResult(PermissionController.createRequestPermissionResultContract()) { granted ->
        scope.launch { AppGraph.syncHealth() }
    }
    val last7 = remember(days) {
        val map = days.associate { it.date to it.steps }
        (6 downTo 0).map { LocalDate.now().minusDays(it.toLong()) }.map { d -> d to (map[d.year * 10000 + d.monthValue * 100 + d.dayOfMonth] ?: 0) }
    }
    Page("Steps", onBack) {
        ErrorText(msg)
        InfoCard("Last 7 days") {
            val max = (last7.maxOfOrNull { it.second } ?: 0).coerceAtLeast(1000)
            val color = MaterialTheme.colorScheme.primary
            Canvas(Modifier.fillMaxWidth().height(160.dp)) {
                val w = size.width / last7.size
                last7.forEachIndexed { i, (_, v) ->
                    val h = size.height * v / max
                    drawRect(color, Offset(i * w + w * 0.15f, size.height - h), Size(w * 0.7f, h))
                }
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                last7.forEach { (d, v) -> Column(horizontalAlignment = Alignment.CenterHorizontally) { Text(d.dayOfWeek.name.take(3), style = MaterialTheme.typography.labelSmall); Text("$v", style = MaterialTheme.typography.labelSmall) } }
            }
        }
        Button(enabled = phase == LinkPhase.CONNECTED, onClick = { AppGraph.link.requestSteps() }) { Text("Get steps from the watch now") }
        InfoCard("Health Connect") {
            Text("Each day is saved once. Syncing again updates the same day instead of adding a copy.")
            when (HealthSteps.status(ctx)) {
                HcStatus.READY -> Button(onClick = { launcher.launch(HealthSteps.permissions) }) { Text("Allow Health Connect") }
                HcStatus.UPDATE_NEEDED -> Text("Health Connect needs an update from the Play Store.")
                HcStatus.NOT_INSTALLED -> Text(Errors.HC_MISSING)
            }
            OutlinedButton(onClick = { scope.launch { AppGraph.syncHealth() } }) { Text("Save to Health Connect now") }
        }
    }
}

// ---------------------------------------------------------------------------- Watch apps

@Composable
fun WatchAppsScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val apps by AppGraph.watchApps.collectAsState()
    val status by AppGraph.appStatus.collectAsState()
    val phase by AppGraph.link.phase.collectAsState()
    var error by remember { mutableStateOf<String?>(null) }
    val connected = phase == LinkPhase.CONNECTED

    fun install(name: String, src: ByteArray) {
        if (!Regex("(?m)^\\s*function\\s+start\\s*\\(").containsMatchIn(src.decodeToString())) { error = Errors.APP_BAD; return }
        error = null
        AppGraph.appStatus.value = "Sending $name..."
        AppUpload.messages(name, src).forEach { AppGraph.link.send(it) }
    }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        val fileName = ctx.contentResolver.query(uri, null, null, null, null)?.use { c -> if (c.moveToFirst()) c.getString(c.getColumnIndexOrThrow(android.provider.OpenableColumns.DISPLAY_NAME)) else null } ?: "app.lua"
        val name = AppUpload.cleanName(fileName)
        val bytes = ctx.contentResolver.openInputStream(uri)?.use { it.readBytes() }
        if (name.isEmpty() || bytes == null || bytes.size > 60_000) { error = Errors.APP_BAD; return@rememberLauncherForActivityResult }
        install(name, bytes)
    }
    LaunchedEffect(connected) { if (connected) AppGraph.link.send(Message.AppLs) }

    Page("Watch apps", onBack) {
        ErrorText(error)
        if (!connected) Text("Connect your watch first (Home screen).")
        status?.let { Text(it) }
        InfoCard("On your watch") {
            if (apps.isEmpty()) Text("No apps installed yet.")
            apps.forEach { n ->
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween) {
                    Text(n)
                    TextButton(enabled = connected, onClick = { AppGraph.link.send(Message.AppRm(n)) }) { Text("Remove") }
                }
            }
            OutlinedButton(enabled = connected, onClick = { AppGraph.link.send(Message.AppLs) }) { Text("Refresh") }
        }
        InfoCard("Add an app") {
            LuaCatalog.entries.forEach { e ->
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) { Text(e.title, style = MaterialTheme.typography.titleSmall); Text(e.blurb, style = MaterialTheme.typography.bodySmall) }
                    Button(enabled = connected, onClick = {
                        val src = ctx.assets.open(e.assetPath).use { it.readBytes() }
                        install(e.name, src)
                    }) { Text(if (e.name in apps) "Update" else "Install") }
                }
            }
            OutlinedButton(enabled = connected, onClick = { picker.launch(arrayOf("*/*")) }) { Text("Pick a .lua file from my phone") }
        }
    }
}
