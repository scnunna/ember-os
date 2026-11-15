package com.nunna.embercompanion.android

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.service.notification.StatusBarNotification
import com.nunna.embercompanion.link.LinkController
import com.nunna.embercompanion.link.LinkPhase
import com.nunna.embercompanion.link.PttClip
import com.nunna.embercompanion.link.TranslateFlow
import com.nunna.embercompanion.link.TranslateResult
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
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

/** The app-wide brain: one link to the watch plus every feature that reacts to what the watch says. */
object AppGraph {
    lateinit var app: Context
        private set
    lateinit var prefs: Prefs
    lateinit var steps: StepsStore
    lateinit var practice: PracticeStore
    lateinit var notes: NotesStore
    lateinit var models: ModelStore
    lateinit var stt: SherpaStt
    lateinit var link: LinkController
    lateinit var gatt: EmberGatt
    lateinit var ringer: Ringer
    private lateinit var tts: Tts
    private lateinit var wifi: WifiFetcher
    val translator = TranslateEngine()

    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
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

    private var wifiWait: CompletableDeferred<Message.Wifi>? = null
    private val jobLock = Mutex()
    private lateinit var flow: TranslateFlow

    @Synchronized
    fun init(context: Context) {
        if (started) return
        started = true
        app = context.applicationContext
        prefs = Prefs(app); steps = StepsStore(app); practice = PracticeStore(app); notes = NotesStore(app)
        models = ModelStore(app); stt = SherpaStt(models); ringer = Ringer(app); tts = Tts(app); wifi = WifiFetcher(app)
        link = LinkController(
            transport = object : com.nunna.embercompanion.link.LinkTransport {
                override fun write(packets: List<ByteArray>) = gatt.write(packets)
            },
            nowMs = { System.currentTimeMillis() },
            tzOffsetSeconds = { TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 1000 },
            languages = { prefs.languages.value },
        )
        gatt = EmberGatt(app) { link }
        flow = TranslateFlow(
            transcribe = { s, lang -> withContext(Dispatchers.Default) { stt.transcribe(s, lang) } },
            translate = { t, f, to -> translator.translate(t, f, to) },
            speak = { t, lang -> tts.speak(t, lang) },
            send = { link.send(it) },
        )
        link.onMessage = { m -> scope.launch { handle(m) } }
        link.onPttClip = { clip -> scope.launch { handleClip(clip) } }
        link.onReadyHook = { scope.launch { delay(300); link.send(Message.AppLs); link.send(Message.RecsReq) } }
    }

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
                watchError.value = "${Errors.WATCH_ERR}: ${m.msg}. Try again; if it keeps happening, restart the watch."
                if (appStatus.value?.startsWith("Sending") == true) appStatus.value = watchError.value
            }
            is Message.Ptt -> pttLive.value = if (m.on) m.side else null
            else -> {}
        }
    }

    fun stopRinging() { ringer.stop(); ringing.value = false }

    private suspend fun handleClip(clip: PttClip) {
        delay(400) // let the last AUDIO packets arrive
        val samples = clip.samples
        val f = Resampler.toFloat(samples, clip.rate, 16000)
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

    /** Download (if needed), transcribe, summarize and save one recording as a note. */
    fun processRecording(rec: Message.RecFile) {
        scope.launch {
            jobLock.withLock {
                val name = rec.n
                try {
                    if (!stt.ready) { setJob(name, RecJob("", error = Errors.STT_MODEL)); return@withLock }
                    val file = recFile(name)
                    if (!file.exists()) download(name, file)
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
                    val llmPath = prefs.llmModelPath?.takeIf { File(it).exists() }
                    val summarizer: Summarizer = if (llmPath != null) LlmSummarizer(app, llmPath) else ExtractiveSummarizer()
                    val sum = summarizer.summarize(text)
                    val note = NoteRecord(name, sum.title, System.currentTimeMillis(), segs, rec.marks, sum.summary, sum.actions, sum.engine)
                    notes.save(note)
                    val llmErr = (summarizer as? LlmSummarizer)?.lastError
                    setJob(name, RecJob("Done", 100, error = llmErr, done = true))
                } catch (e: Exception) {
                    setJob(name, RecJob("", error = userMessage(e.message, Errors.STT_FAIL)))
                }
            }
        }
    }

    private suspend fun download(name: String, dest: File) {
        val wait = CompletableDeferred<Message.Wifi>()
        wifiWait = wait
        try {
            setJob(name, RecJob("Asking the watch to start Wi-Fi"))
            link.send(Message.WifiReq)
            val w = withTimeoutOrNull(20_000) { wait.await() } ?: throw IllegalStateException(Errors.WIFI_WATCH)
            if (w.err != null || w.ssid.isBlank()) throw IllegalStateException(Errors.WIFI_WATCH)
            setJob(name, RecJob("Joining the watch Wi-Fi"))
            wifi.download(w.ssid, w.pass, w.ip.ifBlank { "192.168.4.1" }, name, dest) { p -> setJob(name, RecJob("Downloading", p)) }
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
