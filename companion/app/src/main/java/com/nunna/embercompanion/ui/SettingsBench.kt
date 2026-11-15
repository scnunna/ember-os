package com.nunna.embercompanion.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.nunna.embercompanion.android.*
import com.nunna.embercompanion.protocol.BenchReport
import com.nunna.embercompanion.protocol.BenchRow
import kotlinx.coroutines.launch
import java.io.File

@Composable
fun SettingsScreen(go: (Screen) -> Unit, onBack: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var err by remember { mutableStateOf<String?>(null) }
    var keepDays by remember { mutableIntStateOf(AppGraph.prefs.keepAudioDays) }
    var delWatch by remember { mutableStateOf(AppGraph.prefs.deleteFromWatch) }
    var sttId by remember { mutableStateOf(AppGraph.prefs.sttModelId) }
    var copying by remember { mutableStateOf(false) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        copying = true; err = null
        scope.launch {
            try {
                val name = ctx.contentResolver.query(uri, null, null, null, null)?.use { c -> if (c.moveToFirst()) c.getString(c.getColumnIndexOrThrow(android.provider.OpenableColumns.DISPLAY_NAME)) else null } ?: "model.litertlm"
                if (!name.endsWith(".litertlm")) { err = "LM-02: Pick a model file ending in .litertlm."; copying = false; return@launch }
                val dest = File(ctx.filesDir, "llm/$name")
                kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                    dest.parentFile?.mkdirs()
                    ctx.contentResolver.openInputStream(uri)!!.use { i -> dest.outputStream().use { o -> i.copyTo(o) } }
                }
                AppGraph.prefs.llmModelPath = dest.absolutePath
            } catch (e: Exception) { err = "LM-03: The model file could not be copied. Make sure there is enough free space." }
            copying = false
        }
    }
    Page("Settings", onBack) {
        InfoCard("Ember Gateway (internet)") {
            Text("This app has no internet permission at all. Downloads go through the separate Ember Gateway app, which asks you first and keeps a log.")
            Text(if (AppGraph.gateway.installed()) "Ember Gateway is installed." else Errors.GW_MISSING)
            Button(enabled = AppGraph.gateway.installed(), onClick = { AppGraph.gateway.openApprovals(ctx) }) { Text("Open Ember Gateway (approve / log)") }
        }
        LifeOsCard()
        InfoCard("Recordings (privacy)") {
            Text("Raw audio of a meeting is the most sensitive thing here.")
            Row { RadioButton(selected = keepDays == 0, onClick = { keepDays = 0; AppGraph.prefs.keepAudioDays = 0 }); Text("Delete the recording after it is turned into text (recommended)", Modifier.padding(top = 12.dp)) }
            Row { RadioButton(selected = keepDays == 7, onClick = { keepDays = 7; AppGraph.prefs.keepAudioDays = 7 }); Text("Keep it for 7 days, then delete", Modifier.padding(top = 12.dp)) }
            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Switch(checked = delWatch, onCheckedChange = { delWatch = it; AppGraph.prefs.deleteFromWatch = it })
                Spacer(Modifier.width(8.dp)); Text("Delete from the watch after a safe download")
            }
            Text("Notes, transcripts, steps and practice data are always stored encrypted. Kept audio files are in the app's private storage but are not separately encrypted.", style = MaterialTheme.typography.bodySmall)
        }
        InfoCard("Speech models") {
            SpeechModels.all.forEach { m ->
                Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                    RadioButton(selected = sttId == m.id, onClick = { sttId = m.id; AppGraph.prefs.sttModelId = m.id })
                    Text(m.name + if (AppGraph.sttStores.getValue(m.id).installed()) " (downloaded)" else "")
                }
            }
            ModelCard()
        }
        LlmModelCard("Used for travel translation and for better meeting summaries. Without it, summaries use the simple built-in method and translation is unavailable.")
        InfoCard("Already have a model file?") {
            Text("You can pick a Gemma .litertlm file you already have instead of downloading.")
            ErrorText(err)
            Button(enabled = !copying, onClick = { picker.launch(arrayOf("*/*")) }) { Text(if (copying) "Copying..." else "Choose a model file") }
        }
        InfoCard("What can my phone run?") {
            Text("Measure speech, the AI model and the voice on this phone.")
            Button(onClick = { go(Screen.BENCH) }) { Text("Open the speed test") }
        }
        InfoCard("Permissions") {
            Button(onClick = { ctx.startActivity(EmberNotificationListener.settingsIntent()) }) { Text("Notification access") }
            OutlinedButton(onClick = { ctx.startActivity(Intent(android.provider.Settings.ACTION_BLUETOOTH_SETTINGS)) }) { Text("Bluetooth settings") }
            OutlinedButton(onClick = { ctx.startActivity(Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS, android.net.Uri.parse("package:" + ctx.packageName))) }) { Text("App permissions") }
        }
        InfoCard("About") {
            Text("Ember Companion 0.2.0. Everything stays on your phone: no accounts, no tracking, no internet permission.")
        }
    }
}

@Composable
fun BenchScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val rows = remember { mutableStateListOf<BenchRow>() }
    var running by remember { mutableStateOf<String?>(null) }
    Page("What can my phone run?", onBack) {
        Text("One tap runs real tests on this phone: each is run 3 times and the middle result is shown. Failures are shown as failures. The AI test loads a 2.4 GB model several times, so it takes a few minutes; keep the screen on and the phone plugged in.")
        Button(enabled = running == null, onClick = {
            rows.clear()
            scope.launch {
                val b = BenchRunner(ctx)
                rows += b.deviceInfo()
                for (store in AppGraph.sttStores.values) { running = "Speech: ${store.spec.name}"; rows += b.stt(store) }
                val have = AppGraph.llmModels.path() != null
                for (be in LlmBackend.values()) { running = "AI model on ${be.name}"; rows += b.llm(AppGraph.llm, have, be) }
                running = "Voice"; rows += b.tts(AppGraph.tts)
                running = null
            }
        }) { Text(if (running == null) "Run everything" else "Running...") }
        running?.let { Text("Now: $it"); LinearProgressIndicator(Modifier.fillMaxWidth()) }
        rows.forEach { r ->
            if (r.ok) InfoCard(r.item) { Text(r.details) } else Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
                Column(Modifier.padding(16.dp)) { Text(r.item, style = MaterialTheme.typography.titleMedium); Text("FAILED: ${r.error}") }
            }
        }
        if (rows.isNotEmpty()) OutlinedButton(onClick = {
            val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("Ember bench", BenchReport.format(rows.toList())))
        }) { Text("Copy results") }
    }
}

@Composable
private fun LifeOsCard() {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val status by AppGraph.lifeStatus.collectAsState()
    val err by AppGraph.link.lifeError.collectAsState()
    var ciOn by remember { mutableStateOf(AppGraph.prefs.ciOn) }
    var snooze by remember { mutableIntStateOf(AppGraph.prefs.ciSnooze) }
    LaunchedEffect(Unit) { AppGraph.refreshLifeStatus() }
    fun pushCfg() { AppGraph.link.send(AppGraph.prefs.ciCfg()) }
    InfoCard("Life OS link (watch check-ins)") {
        Text("Amber checks in on the watch at the times set in Life OS and the answers go back to Life OS through Ember Gateway.")
        Text(
            when {
                status == null -> "Checking..."
                !status!!.gatewayInstalled -> Errors.GW_MISSING
                status!!.configured -> "Life OS link: configured (${status!!.host})."
                else -> "Life OS link: not configured. Open Ember Gateway and enter the address and token."
            },
        )
        err?.let { Text("Last problem: " + when (it) {
            "OFFLINE" -> "could not reach Life OS (it will retry)."
            "NOT_CONFIGURED" -> "the link is not set up in Ember Gateway."
            "HTTP_ERROR" -> "Life OS refused the request (check the token)."
            else -> it
        }, style = MaterialTheme.typography.bodySmall) }
        Text("Waiting to send: ${AppGraph.link.pendingCheckins()} check-ins", style = MaterialTheme.typography.bodySmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(enabled = status?.gatewayInstalled == true, onClick = { AppGraph.gateway.openLifeOsSettings(ctx) }) { Text("Open Ember Gateway settings") }
            OutlinedButton(onClick = { scope.launch { AppGraph.refreshLifeStatus() } }) { Text("Refresh") }
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Switch(checked = ciOn, onCheckedChange = { ciOn = it; AppGraph.prefs.ciOn = it; pushCfg() })
            Spacer(Modifier.width(8.dp)); Text("Check-ins on the watch")
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Snooze")
            listOf(5, 15, 30, 60).forEach { v ->
                FilterChip(selected = snooze == v, onClick = { snooze = v; AppGraph.prefs.ciSnooze = v; pushCfg() }, label = { Text("$v min") })
            }
        }
    }
}
