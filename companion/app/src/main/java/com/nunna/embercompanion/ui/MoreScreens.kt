package com.nunna.embercompanion.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.nunna.embercompanion.android.*
import com.nunna.embercompanion.link.LinkPhase
import com.nunna.embercompanion.protocol.Message
import com.nunna.embercompanion.protocol.PracticeCalc
import kotlinx.coroutines.launch
import java.io.File
import java.time.LocalDate

// ---------------------------------------------------------------------------- Travel translate

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LangPicker(label: String, code: String, onPick: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    ExposedDropdownMenuBox(expanded = open, onExpandedChange = { open = it }) {
        OutlinedTextField(
            value = Languages.name(code), onValueChange = {}, readOnly = true, label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(open) },
            modifier = Modifier.menuAnchor().fillMaxWidth(),
        )
        ExposedDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            Languages.codes.forEach { c -> DropdownMenuItem(text = { Text(Languages.name(c)) }, onClick = { onPick(c); open = false }) }
        }
    }
}

@Composable
fun TranslateScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val langs by AppGraph.prefs.languages.collectAsState()
    val phase by AppGraph.link.phase.collectAsState()
    val busy by AppGraph.translateBusy.collectAsState()
    val last by AppGraph.lastTranslation.collectAsState()
    val live by AppGraph.pttLive.collectAsState()
    var packs by remember { mutableStateOf<Set<String>?>(null) }
    var packError by remember { mutableStateOf<String?>(null) }
    var downloading by remember { mutableStateOf<String?>(null) }
    var micError by remember { mutableStateOf<String?>(null) }
    var recording by remember { mutableStateOf<String?>(null) }
    val modelState by AppGraph.models.state.collectAsState()

    LaunchedEffect(Unit) { packs = runCatching { AppGraph.translator.downloaded() }.getOrNull() }
    fun setLangs(me: String, them: String) {
        AppGraph.prefs.setLanguages(me, them)
        AppGraph.link.send(Message.TrCfg(me, them))
    }
    val micPerm = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { ok -> if (!ok) micError = Errors.MIC else micError = null }
    fun hasMic() = ContextCompat.checkSelfPermission(ctx, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    Page("Travel translate", onBack) {
        LangPicker("My language", langs.me) { setLangs(it, langs.them) }
        LangPicker("Their language", langs.them) { setLangs(langs.me, it) }
        InfoCard("Language packs") {
            Text("Each pack is about 30 MB and is downloaded once, from Google. After that translation works offline.")
            for (c in listOf(langs.me, langs.them).distinct()) {
                val has = packs?.contains(c) == true || c == "en" && packs?.contains("en") == true
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween) {
                    Text(Languages.name(c) + if (has) " - ready" else " - not downloaded")
                    if (!has) Button(enabled = downloading == null, onClick = {
                        downloading = c; packError = null
                        scope.launch {
                            try { AppGraph.translator.download(c); packs = AppGraph.translator.downloaded() }
                            catch (e: Exception) { packError = "TR-07: The ${Languages.name(c)} pack did not download. Check your internet and try again." }
                            downloading = null
                        }
                    }) { Text(if (downloading == c) "Downloading..." else "Download") }
                }
            }
            ErrorText(packError)
        }
        if (modelState !is DownloadState.Done) ModelCard()
        ErrorText(micError)
        InfoCard("With the watch") {
            Text(if (phase == LinkPhase.CONNECTED) "Open Translate on the watch, hold the button and speak. The answer is spoken here and shown on the watch." else "Connect your watch to use it here.")
            live?.let { Text("Listening to the watch (${if (it == "me") "you" else "them"})...") }
        }
        InfoCard("With this phone only") {
            Text("Hold a button while talking, then let go.")
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                for ((side, label) in listOf("me" to "I speak", "them" to "They speak")) {
                    val active = recording == side
                    Surface(
                        color = if (active) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary,
                        shape = MaterialTheme.shapes.large,
                        modifier = Modifier.weight(1f).height(72.dp).pointerInput(side, busy) {
                            detectTapGestures(onPress = {
                                if (busy) return@detectTapGestures
                                if (!hasMic()) { micPerm.launch(Manifest.permission.RECORD_AUDIO); return@detectTapGestures }
                                if (AppGraph.micStart()) {
                                    recording = side
                                    tryAwaitRelease()
                                    recording = null
                                    val audio = AppGraph.micStop()
                                    scope.launch { AppGraph.runTranslation(side, audio) }
                                } else micError = Errors.MIC
                            })
                        },
                    ) { Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { Text(if (active) "Listening..." else label, color = MaterialTheme.colorScheme.onPrimary) } }
                }
            }
        }
        if (busy) { Text("Working on it..."); LinearProgressIndicator(Modifier.fillMaxWidth()) }
        last?.let { r ->
            ErrorText(r.error)
            if (r.heard.isNotBlank()) InfoCard(if (r.side == "me") "You said" else "They said") {
                Text(r.heard)
                if (r.translated.isNotBlank()) { Text("Translation", style = MaterialTheme.typography.labelMedium); Text(r.translated, style = MaterialTheme.typography.headlineSmall) }
            }
        }
    }
}

// ---------------------------------------------------------------------------- Guitar practice

@Composable
fun PracticeScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val sessions by AppGraph.practice.sessions.collectAsState()
    val phase by AppGraph.link.phase.collectAsState()
    val st = remember(sessions) { PracticeCalc.stats(sessions, LocalDate.now()) }
    var msg by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(phase) { if (phase == LinkPhase.CONNECTED) AppGraph.link.send(Message.PracticeReq) }
    Page("Guitar practice", onBack) {
        InfoCard("This week") {
            Text("${st.weekMinutes} minutes", style = MaterialTheme.typography.headlineMedium)
            Text("Streak: ${st.streakDays} day${if (st.streakDays == 1) "" else "s"} in a row")
            val max = (st.minutesByDay.maxOfOrNull { it.second } ?: 0).coerceAtLeast(10)
            val color = MaterialTheme.colorScheme.primary
            Canvas(Modifier.fillMaxWidth().height(100.dp)) {
                val w = size.width / st.minutesByDay.size
                st.minutesByDay.forEachIndexed { i, (_, v) -> val h = size.height * v / max; drawRect(color, Offset(i * w + w * 0.15f, size.height - h), Size(w * 0.7f, h)) }
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { st.minutesByDay.forEach { (d, _) -> Text(d.dayOfWeek.name.take(1), style = MaterialTheme.typography.labelSmall) } }
        }
        InfoCard("Fastest tempo (BPM) over time") {
            if (st.bpmTrend.size < 2) Text(if (st.bpmTrend.isEmpty()) "No tempo data yet." else "Latest: ${st.bpmTrend.last().second} BPM")
            else {
                val lo = st.bpmTrend.minOf { it.second } - 5
                val hi = st.bpmTrend.maxOf { it.second } + 5
                val color = MaterialTheme.colorScheme.primary
                Canvas(Modifier.fillMaxWidth().height(100.dp)) {
                    val pts = st.bpmTrend.mapIndexed { i, p -> Offset(size.width * i / (st.bpmTrend.size - 1), size.height * (1f - (p.second - lo).toFloat() / (hi - lo))) }
                    for (i in 0 until pts.size - 1) drawLine(color, pts[i], pts[i + 1], strokeWidth = 6f)
                    pts.forEach { drawCircle(color, 8f, it) }
                }
                Text("From ${st.bpmTrend.first().second} to ${st.bpmTrend.last().second} BPM")
            }
        }
        ErrorText(msg)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(enabled = phase == LinkPhase.CONNECTED, onClick = { AppGraph.link.send(Message.PracticeReq) }) { Text("Refresh") }
            Button(onClick = {
                val i = ctx.packageManager.getLaunchIntentForPackage("com.nunna.guitarnoodle")
                if (i == null) msg = "GN-01: GuitarNoodle is not installed on this phone." else ctx.startActivity(i)
            }) { Text("Open GuitarNoodle") }
        }
    }
}

// ---------------------------------------------------------------------------- Settings

@Composable
fun SettingsScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    var llm by remember { mutableStateOf(AppGraph.prefs.llmModelPath) }
    var err by remember { mutableStateOf<String?>(null) }
    var copying by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        copying = true; err = null
        scope.launch {
            try {
                val name = ctx.contentResolver.query(uri, null, null, null, null)?.use { c -> if (c.moveToFirst()) c.getString(c.getColumnIndexOrThrow(android.provider.OpenableColumns.DISPLAY_NAME)) else null } ?: "model.task"
                if (!(name.endsWith(".task") || name.endsWith(".litertlm"))) { err = "LM-02: Pick a model file ending in .task or .litertlm."; copying = false; return@launch }
                val dest = File(ctx.filesDir, "llm/$name")
                kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                    dest.parentFile?.mkdirs()
                    ctx.contentResolver.openInputStream(uri)!!.use { i -> dest.outputStream().use { o -> i.copyTo(o) } }
                }
                AppGraph.prefs.llmModelPath = dest.absolutePath; llm = dest.absolutePath
            } catch (e: Exception) { err = "LM-03: The model file could not be copied. Make sure there is enough free space." }
            copying = false
        }
    }
    Page("Settings", onBack) {
        ModelCard()
        InfoCard("Better summaries (optional)") {
            Text("Meeting summaries work without this. If you already have an on-device AI model file (for example Gemma, ending in .task or .litertlm), you can pick it here and it will be used for summaries. This app never downloads such models for you.")
            ErrorText(err)
            if (llm != null) {
                Text("In use: ${File(llm!!).name}")
                OutlinedButton(onClick = { File(llm!!).delete(); AppGraph.prefs.llmModelPath = null; llm = null }) { Text("Remove it") }
            }
            Button(enabled = !copying, onClick = { picker.launch(arrayOf("*/*")) }) { Text(if (copying) "Copying..." else "Choose a model file") }
        }
        InfoCard("Permissions") {
            Button(onClick = { ctx.startActivity(EmberNotificationListener.settingsIntent()) }) { Text("Notification access") }
            OutlinedButton(onClick = { ctx.startActivity(Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS, android.net.Uri.parse("package:" + ctx.packageName))) }) { Text("App permissions") }
        }
        InfoCard("About") {
            Text("Ember Companion 0.1.0. Everything stays on your phone: no accounts, no tracking. Internet is only used when you tap a Download button.")
        }
    }
}
