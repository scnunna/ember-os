package com.nunna.embercompanion.android

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.service.notification.StatusBarNotification
import com.nunna.embercompanion.assistant.Confirmer
import com.nunna.embercompanion.assistant.ToolCall
import com.nunna.embercompanion.assistant.ToolGuard
import com.nunna.embercompanion.assistant.ToolSpec
import com.nunna.embercompanion.link.CheckinLog
import com.nunna.embercompanion.link.LinkController
import com.nunna.embercompanion.link.LinkPhase
import com.nunna.embercompanion.link.PttClip
import com.nunna.embercompanion.link.RecordingPolicy
import com.nunna.embercompanion.link.TranslateFlow
import com.nunna.embercompanion.link.TranslateResult
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.File
import java.time.Instant
import java.time.ZoneId
import java.util.TimeZone

data class RecJob(val stage: String, val percent: Int = 0, val error: String? = null, val done: Boolean = false)

/** A side-effect action waiting for the user's tap (S6). */
class PendingConfirm(val spec: ToolSpec, val call: ToolCall, val answer: CompletableDeferred<Boolean>)

/** The app-wide brain: one link to the watch plus every feature that reacts to what the watch says. */
object AppGraph {
    lateinit var app: Context
        private set
    lateinit var prefs: Prefs
    lateinit var steps: StepsStore
    lateinit var practice: PracticeStore
    lateinit var notes: NotesStore
    lateinit var gateway: GatewayClient
    lateinit var sttStores: Map<String, ModelStore>
    lateinit var llmModels: LlmModelStore
    lateinit var llm: LlmRuntime
    lateinit var stt: SherpaStt
    lateinit var link: LinkController
    lateinit var checkins: CheckinLog
    lateinit var amber: com.nunna.embercompanion.link.AmberFlow
    val lifeStatus = MutableStateFlow<com.nunna.embercompanion.link.LifeOsStatus?>(null)
    lateinit var gatt: EmberGatt
    lateinit var ringer: Ringer
    lateinit var translator: Translator
    lateinit var guard: ToolGuard
    lateinit var tts: Tts
        private set

    /** The speech model the user picked (tiny by default). */
    val models: ModelStore get() = sttStores.getValue(prefs.sttModelId)

    // A failing background job (one message, one Gateway call) is logged instead of killing the app, which would also
    // drop the Bluetooth link to the watch.
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default + kotlinx.coroutines.CoroutineExceptionHandler { _, e ->
        android.util.Log.e("EmberApp", "background job failed", e)
        runCatching { com.nunna.embercompanion.crashLog.record("CAUGHT", e) }
    })
    private var started = false

    val recs = MutableStateFlow<List<Message.RecFile>>(emptyList())
    val recJobs = MutableStateFlow<Map<String, RecJob>>(emptyMap())
    val watchApps = MutableStateFlow<List<String>>(emptyList())
    val appStatus = MutableStateFlow<String?>(null)
    val watchError = MutableStateFlow<String?>(null)
    val healthMessage = MutableStateFlow<String?>(null)
    val ringing = MutableStateFlow(false)
    val translateBusy = MutableStateFlow(false)
    val lastTranslation = MutableStateFlow<TranslateResult?>(null)
    val pttLive = MutableStateFlow<String?>(null)
    val confirm = MutableStateFlow<PendingConfirm?>(null)
    val needsGatewayApproval = MutableStateFlow(false)

    private var wifiWait: CompletableDeferred<Message.Wifi>? = null
    private val jobLock = Mutex()
    private lateinit var flow: TranslateFlow

    @Synchronized
    fun init(context: Context) {
        if (started) return
        started = true
        app = context.applicationContext
        prefs = Prefs(app); steps = StepsStore(app); practice = PracticeStore(app); notes = NotesStore(app)
        gateway = GatewayClient(app)
        sttStores = SpeechModels.all.associate { it.id to ModelStore(app, gateway, it) }
        llmModels = LlmModelStore(app, gateway) { prefs }
        llm = LlmRuntime(app) { llmModels.path() }
        stt = SherpaStt { models }
        ringer = Ringer(app); tts = Tts(app)
        translator = LlmTranslator(llm)
        val ciPersist = CheckinPersistence(app)
        checkins = CheckinLog(ciPersist.load(), { ciPersist.save(it) })
        link = LinkController(
            transport = object : com.nunna.embercompanion.link.LinkTransport {
                override fun write(packets: List<ByteArray>) = gatt.write(packets)
            },
            nowMs = { System.currentTimeMillis() },
            tzOffsetSeconds = { TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 1000 },
            languages = { prefs.languages.value },
            phoneName = {
                android.provider.Settings.Global.getString(app.contentResolver, android.provider.Settings.Global.DEVICE_NAME)
                    ?: android.os.Build.MODEL
            },
            lifeOs = gateway,
            checkinLog = checkins,
            ciConfig = { prefs.ciCfg() },
        )
        gatt = EmberGatt(app) { link }
        amber = com.nunna.embercompanion.link.AmberFlow(
            api = gateway, send = { link.send(it) }, connected = { link.phase.value == LinkPhase.CONNECTED },
            nowMs = { System.currentTimeMillis() }, tzOffsetSeconds = { TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 1000 },
        )
        flow = TranslateFlow(
            transcribe = { s, lang -> withContext(Dispatchers.Default) { stt.transcribe(s, lang) } },
            translate = { t, f, to -> translator.translate(t, f, to) },
            speak = { t, lang -> tts.speak(t, lang) },
            send = { link.send(it) },
        )
        guard = ToolGuard(buildTools(), Confirmer { spec, call -> askUser(spec, call) })
        link.onMessage = { m -> scope.launch { handle(m) } }
        link.onPttClip = { clip -> scope.launch { handleClip(clip) } }
        link.onReadyHook = { scope.launch { delay(300); link.send(Message.AppLs); link.send(Message.RecsReq) } }
        scope.launch { purgeOldAudio() }
    }

    // ---------------------------------------------------------------- S6: tools that change things always ask

    private suspend fun askUser(spec: ToolSpec, call: ToolCall): Boolean {
        val p = PendingConfirm(spec, call, CompletableDeferred())
        confirm.value = p
        val ok = withTimeoutOrNull(120_000) { p.answer.await() } ?: false
        confirm.value = null
        return ok
    }

    private fun buildTools(): List<ToolSpec> = listOf(
        ToolSpec("list_notes", "List saved meeting notes", false) { notes.notes.value.joinToString("\n") { "${it.id}: ${it.title}" } },
        ToolSpec("send_reply", "Send a quick reply to a notification", true) { a ->
            NotifBridge.reply(app, a["id"]?.toIntOrNull() ?: error("no id"), a["idx"]?.toIntOrNull() ?: 0); "sent"
        },
        ToolSpec("delete_note", "Delete a saved note", true) { a -> notes.delete(a["id"] ?: error("no id")); "deleted" },
        ToolSpec("send_note_to_watch", "Send a note summary to the watch", true) { a ->
            sendNote(notes.get(a["id"] ?: error("no id")) ?: error("no such note")); "sent"
        },
        ToolSpec("install_app", "Install a bundled app on the watch", true) { a ->
            val e = LuaCatalog.entries.firstOrNull { it.name == a["name"] } ?: error("no such app")
            AppUpload.messages(e.name, app.assets.open(e.assetPath).use { it.readBytes() }).forEach { link.send(it) }; "sent"
        },
    )

    // ---------------------------------------------------------------- watch -> phone

    private suspend fun handle(m: Message) {
        when (m) {
            is Message.Steps -> { steps.update(m.days); syncHealth() }
            is Message.Reply -> NotifBridge.reply(app, m.id, m.idx)
            is Message.Dismiss -> NotifBridge.dismiss(m.id)
            is Message.Recs -> recs.value = m.files
            is Message.Wifi -> wifiWait?.complete(m)
            is Message.Practice -> practice.merge(m.sessions)
            is Message.FindPhone -> if (m.on) { ringer.start(); ringing.value = true } else stopRinging()
            is Message.MusicCmd -> Media.command(app, m.cmd)
            is Message.Apps -> { watchApps.value = m.list; appStatus.value = null }
            is Message.AppOk -> { appStatus.value = "Installed ${m.name} on the watch."; link.send(Message.AppLs) }
            is Message.Err -> {
                val denied = m.msg.contains("denied", true)
                if (m.re == "wifi_req") wifiWait?.complete(Message.Wifi("", "", "", m.msg.ifBlank { "denied on watch" }))
                watchError.value = if (denied) "WA-02: You did not tap Allow on the watch in time (${m.re.ifBlank { "request" }}). Try again and tap Allow on the watch."
                else "${Errors.WATCH_ERR}${if (m.re.isNotBlank()) " (${m.re})" else ""}: ${m.msg}. Try again; if it keeps happening, restart the watch."
                if (appStatus.value?.startsWith("Sending") == true) appStatus.value = watchError.value
            }
            is Message.Hello, is Message.Ci -> link.lifeTick()
            is Message.Ptt -> pttLive.value = if (m.on) m.side else null
            else -> {}
        }
    }

    /** Asks Ember Gateway whether the Life OS link is set up (Settings screen). */
    suspend fun refreshLifeStatus() { lifeStatus.value = gateway.status() }

    fun stopRinging() { ringer.stop(); ringing.value = false }

    private suspend fun handleClip(clip: PttClip) {
        // Wait until every block the watch said it sent has arrived, but never more than ~400 ms.
        var waited = 0
        while (!clip.complete && waited < 400) { delay(20); waited += 20 }
        val f = Resampler.toFloat(clip.samples, clip.rate, 16000)
        if (clip.side == "amber") { talkToAmber(f, clip.ctx); return }
        runTranslation(clip.side, f)
    }

    // ---------------------------------------------------------------- Health Connect

    suspend fun syncHealth() {
        try {
            if (HealthSteps.status(app) != HcStatus.READY) { healthMessage.value = Errors.HC_MISSING; return }
            if (!HealthSteps.hasPermission(app)) { healthMessage.value = Errors.HC_PERMISSION; return }
            val specs = StepsMapping.toRecords(steps.days.value, ZoneId.systemDefault(), Instant.now())
            HealthSteps.write(app, specs)
            healthMessage.value = null
        } catch (e: Exception) {
            healthMessage.value = Errors.HC_WRITE
        }
    }

    // ---------------------------------------------------------------- recordings -> notes

    fun recFile(name: String) = File(app.filesDir, "recs/$name")

    private fun setJob(name: String, j: RecJob) = recJobs.update { it + (name to j) }

    /** Deletes kept recordings that are past the retention choice (and any leftovers when the choice is "delete"). */
    private fun purgeOldAudio() {
        val dir = File(app.filesDir, "recs")
        val now = System.currentTimeMillis()
        dir.listFiles()?.forEach { f ->
            if (f.name.endsWith(".part")) f.delete()
            else if (RecordingPolicy.expired(f.lastModified(), now, prefs.keepAudioDays.coerceAtLeast(2))) f.delete() // a failed note keeps its audio 2 days so it can be retried
        }
    }

    /** Download (if needed), transcribe, summarize and save one recording as a note. */
    fun processRecording(rec: Message.RecFile) {
        scope.launch {
            jobLock.withLock {
                val name = rec.n
                try {
                    if (!stt.ready) { setJob(name, RecJob("", error = Errors.STT_MODEL)); return@withLock }
                    val file = recFile(name)
                    if (!file.exists()) download(rec, file)
                    setJob(name, RecJob("Reading the recording"))
                    val wav = withContext(Dispatchers.IO) { WavReader.read(file.readBytes()) }
                    val audio = Resampler.toFloat(wav.samples, wav.sampleRate, 16000)
                    setJob(name, RecJob("Turning speech into text", 0))
                    val lang = prefs.languages.value.me
                    val segs = withContext(Dispatchers.Default) {
                        stt.transcribeLong(audio, lang) { p -> setJob(name, RecJob("Turning speech into text", p)) }
                    }
                    if (segs.isEmpty()) { setJob(name, RecJob("", error = "ST-04: No speech was found in this recording. Check the watch microphone and try a new recording.")); return@withLock }
                    setJob(name, RecJob("Writing the summary"))
                    val text = segs.joinToString(" ") { it.text }
                    val summarizer: Summarizer = if (llmModels.path() != null) LlmSummarizer(llm) else ExtractiveSummarizer()
                    val sum = summarizer.summarize(text)
                    val note = NoteRecord(name, sum.title, System.currentTimeMillis(), segs, rec.marks, sum.summary, sum.actions, sum.engine)
                    notes.save(note)
                    // S7: raw audio is deleted once it has been turned into text (unless the user chose to keep it for a few days).
                    if (RecordingPolicy.deleteAfterTranscription(prefs.keepAudioDays)) file.delete()
                    setJob(name, RecJob("Done", 100, error = (summarizer as? LlmSummarizer)?.lastError, done = true))
                } catch (e: Exception) {
                    setJob(name, RecJob("", error = userMessage(e.message, Errors.STT_FAIL)))
                }
            }
        }
    }

    private suspend fun download(rec: Message.RecFile, dest: File) {
        val name = rec.n
        val wait = CompletableDeferred<Message.Wifi>()
        wifiWait = wait
        try {
            setJob(name, RecJob("Asking the watch to start Wi-Fi (tap Allow on the watch)"))
            link.send(Message.WifiReq)
            val w = withTimeoutOrNull(45_000) { wait.await() } ?: throw IllegalStateException(Errors.WIFI_WATCH)
            if (w.err != null || w.ssid.isBlank() || w.token.isBlank()) throw IllegalStateException(Errors.WIFI_WATCH)
            setJob(name, RecJob("Joining the watch Wi-Fi"))
            val r = gateway.fetchWatchFile(
                w.ssid, w.pass, w.ip.ifBlank { "192.168.4.1" }, name, w.token, rec.s, "Recording $name",
                onProgress = { p -> setJob(name, RecJob("Downloading", p)) }, onNeedsApproval = {},
            )
            if (!r.ok || r.uri == null) throw IllegalStateException(r.userMessage())
            withContext(Dispatchers.IO) {
                dest.parentFile?.mkdirs()
                val tmp = File(dest.path + ".part")
                try {
                    val input = gateway.open(r.uri) ?: throw IllegalStateException(Errors.GW_FAIL)
                    input.use { i -> tmp.outputStream().use { o -> i.copyTo(o) } }
                    // Only a complete file counts: the size must match what the watch listed.
                    if (!RecordingPolicy.verify(rec.s, tmp.length())) throw IllegalStateException(Errors.WIFI_SIZE)
                    dest.delete()
                    if (!tmp.renameTo(dest)) throw IllegalStateException(Errors.GW_FAIL)
                } finally {
                    tmp.delete()
                    gateway.delete(r.uri)
                }
            }
            link.send(Message.WifiOff)
            // Verified: let the watch delete its copy (it checks the size too).
            RecordingPolicy.recDel(rec, dest.length(), prefs.deleteFromWatch)?.let { link.send(it) }
        } finally {
            wifiWait = null
            link.send(Message.WifiOff)
        }
    }

    fun sendNote(n: NoteRecord) {
        link.send(n.toWatchMessage())
        notes.save(n.copy(sentToWatch = true))
    }

    // ---------------------------------------------------------------- translation

    suspend fun runTranslation(side: String, audio16k: FloatArray) {
        translateBusy.value = true
        try {
            val l = prefs.languages.value
            lastTranslation.value = flow.run(side, audio16k, l.me, l.them)
        } finally {
            translateBusy.value = false
        }
    }

    // ---------------------------------------------------------------- Talk to Amber (voice from the watch)

    /** Whisper on the phone (same path as Translate), then the text goes to Amber through Ember Gateway. Audio never leaves the phone. */
    private suspend fun talkToAmber(audio16k: FloatArray, ctx: Message.PttCtx?) {
        if (!stt.ready) { link.send(Message.Amber("local", "The phone has no speech model yet. Download it in Companion Settings.", true)); return }
        val text = try { withContext(Dispatchers.Default) { stt.transcribe(audio16k, prefs.languages.value.me) } } catch (e: Exception) { "" }
        amber.heardVoice(text, ctx)
    }

    // ---------------------------------------------------------------- phone microphone (phone-only translate mode)

    private var mic: AudioRecord? = null
    private var micThread: Thread? = null
    @Volatile private var micRun = false
    private val micBuf = java.io.ByteArrayOutputStream()

    /** Caller must hold the RECORD_AUDIO permission. */
    @SuppressLint("MissingPermission")
    fun micStart(): Boolean {
        if (micRun) return true
        val min = AudioRecord.getMinBufferSize(16000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val r = AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, 16000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(min, 16000))
        if (r.state != AudioRecord.STATE_INITIALIZED) { r.release(); return false }
        mic = r; micBuf.reset(); micRun = true
        r.startRecording()
        micThread = Thread {
            val b = ByteArray(4096)
            while (micRun) { val n = r.read(b, 0, b.size); if (n > 0) synchronized(micBuf) { micBuf.write(b, 0, n) } }
        }.also { it.start() }
        return true
    }

    /** Stops recording and returns 16 kHz mono float audio. */
    fun micStop(): FloatArray {
        micRun = false
        micThread?.join(500)
        mic?.let { runCatching { it.stop() }; it.release() }
        mic = null
        val bytes = synchronized(micBuf) { micBuf.toByteArray() }
        val s = ShortArray(bytes.size / 2) { ((bytes[2 * it].toInt() and 0xFF) or (bytes[2 * it + 1].toInt() shl 8)).toShort() }
        return Resampler.toFloat(s, 16000, 16000)
    }

    // ---------------------------------------------------------------- helpers for the UI

    fun connected() = link.phase.value == LinkPhase.CONNECTED
}

/** Our own errors start with a code like `WF-01:`; anything else gets the plain fallback sentence. */
fun userMessage(msg: String?, fallback: String): String =
    if (msg != null && Regex("^[A-Z]{2}-[0-9]{2}:").containsMatchIn(msg)) msg else fallback

/** Remembers the notifications we forwarded so a watch reply/dismiss can find the original. */
object NotifBridge {
    private val sent = LinkedHashMap<Int, StatusBarNotification>()
    private var next = 1

    @Synchronized fun register(sbn: StatusBarNotification): Int {
        val existing = sent.entries.firstOrNull { it.value.key == sbn.key }?.key
        val id = existing ?: next++
        sent[id] = sbn
        while (sent.size > 40) sent.remove(sent.keys.first())
        return id
    }

    @Synchronized fun idFor(key: String): Int? = sent.entries.firstOrNull { it.value.key == key }?.key

    @Synchronized fun get(id: Int) = sent[id]

    fun quickReplies() = listOf("OK", "On my way", "Call you later")

    fun reply(ctx: Context, id: Int, idx: Int) {
        val sbn = get(id) ?: return
        val text = quickReplies().getOrNull(idx) ?: return
        val action = sbn.notification.actions?.firstOrNull { a -> a.remoteInputs?.any { it.allowFreeFormInput } == true } ?: return
        val inputs = action.remoteInputs ?: return
        val intent = android.content.Intent()
        val bundle = android.os.Bundle()
        for (ri in inputs) bundle.putCharSequence(ri.resultKey, text)
        android.app.RemoteInput.addResultsToIntent(inputs, intent, bundle)
        runCatching { action.actionIntent.send(ctx, 0, intent) }
    }

    fun dismiss(id: Int) {
        val sbn = get(id) ?: return
        EmberNotificationListener.cancel(sbn.key)
    }
}
