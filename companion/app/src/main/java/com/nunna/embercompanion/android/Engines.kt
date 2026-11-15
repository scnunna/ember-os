package com.nunna.embercompanion.android

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Debug
import android.view.KeyEvent
import androidx.health.connect.client.HealthConnectClient
import androidx.health.connect.client.permission.HealthPermission
import androidx.health.connect.client.records.StepsRecord
import androidx.health.connect.client.records.metadata.Device
import androidx.health.connect.client.records.metadata.Metadata
import com.google.ai.edge.litertlm.Backend
import com.google.ai.edge.litertlm.Content
import com.google.ai.edge.litertlm.Contents
import com.google.ai.edge.litertlm.ConversationConfig
import com.google.ai.edge.litertlm.Engine
import com.google.ai.edge.litertlm.EngineConfig
import com.google.ai.edge.litertlm.ExperimentalApi
import com.google.ai.edge.litertlm.ExperimentalFlags
import com.google.ai.edge.litertlm.Message
import com.nunna.embercompanion.assistant.Prompts
import com.nunna.embercompanion.protocol.ExtractiveSummarizer
import com.nunna.embercompanion.protocol.LlmFormat
import com.nunna.embercompanion.protocol.LlmRun
import com.nunna.embercompanion.protocol.StepsRecordSpec
import com.nunna.embercompanion.protocol.Summarizer
import com.nunna.embercompanion.protocol.Summary
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.time.ZoneOffset

// ------------------------------------------------------------------ Health Connect

enum class HcStatus { READY, NOT_INSTALLED, UPDATE_NEEDED }

object HealthSteps {
    val permissions = setOf(HealthPermission.getWritePermission(StepsRecord::class))

    fun status(ctx: Context): HcStatus = when (HealthConnectClient.getSdkStatus(ctx)) {
        HealthConnectClient.SDK_AVAILABLE -> HcStatus.READY
        HealthConnectClient.SDK_UNAVAILABLE_PROVIDER_UPDATE_REQUIRED -> HcStatus.UPDATE_NEEDED
        else -> HcStatus.NOT_INSTALLED
    }

    suspend fun hasPermission(ctx: Context) =
        HealthConnectClient.getOrCreate(ctx).permissionController.getGrantedPermissions().containsAll(permissions)

    /** Writes the day records. The per-date clientRecordId makes a repeat update the same entry instead of adding one. */
    suspend fun write(ctx: Context, specs: List<StepsRecordSpec>) {
        if (specs.isEmpty()) return
        val records = specs.map { s ->
            StepsRecord(
                count = s.count,
                startTime = s.start, startZoneOffset = ZoneOffset.ofTotalSeconds(s.zoneOffsetSeconds),
                endTime = s.end, endZoneOffset = ZoneOffset.ofTotalSeconds(s.zoneOffsetSeconds),
                metadata = Metadata.autoRecorded(Device(type = Device.TYPE_WATCH), clientRecordId = s.clientRecordId, clientRecordVersion = s.version),
            )
        }
        HealthConnectClient.getOrCreate(ctx).insertRecords(records)
    }
}

// ------------------------------------------------------------------ On-device LLM (LiteRT-LM, Gemma 4 E2B)

/** Which processor runs the model. */
enum class LlmBackend { CPU, GPU }

/** Translation backend. The shipped one is the on-device Gemma model (ML Kit was dropped: its packs need INTERNET in this app). */
fun interface Translator {
    suspend fun translate(text: String, from: String, to: String): String
}

/**
 * Wraps the LiteRT-LM engine. The engine is big (about 1.5 GB of RAM while loaded), so it is loaded on first use and
 * dropped again after two idle minutes. Everything runs on the phone.
 */
@OptIn(ExperimentalApi::class)
class LlmRuntime(private val ctx: Context, private val modelPath: () -> String?) {
    private val lock = Mutex()
    private var engine: Engine? = null
    private var engineBackend: LlmBackend? = null
    private var idleJob: Job? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private fun backendObj(b: LlmBackend): Backend = if (b == LlmBackend.GPU) Backend.GPU() else Backend.CPU()

    private fun open(path: String, backend: LlmBackend): Engine {
        val e = Engine(EngineConfig(modelPath = path, backend = backendObj(backend), cacheDir = ctx.cacheDir.absolutePath))
        e.initialize()
        return e
    }

    private fun text(m: Message): String =
        m.contents.contents.filterIsInstance<Content.Text>().joinToString("") { it.text }

    /** Runs one prompt (instructions in [system], task + untrusted data in [user]). */
    suspend fun generate(system: String, user: String, backend: LlmBackend = LlmBackend.CPU): String = lock.withLock {
        val path = modelPath() ?: throw IllegalStateException(Errors.LLM_MODEL)
        withContext(Dispatchers.Default) {
            idleJob?.cancel()
            try {
                if (engine == null || engineBackend != backend) { engine?.close(); engine = open(path, backend); engineBackend = backend }
                engine!!.createConversation(ConversationConfig(systemInstruction = Contents.of(system))).use { c -> text(c.sendMessage(user)) }
            } catch (e: Throwable) {
                runCatching { engine?.close() }; engine = null
                throw IllegalStateException(Errors.LLM_RUN)
            } finally {
                idleJob = scope.launch { delay(120_000); lock.withLock { runCatching { engine?.close() }; engine = null } }
            }
        }
    }

    fun release() { runCatching { engine?.close() }; engine = null }

    /** One timed run for the bench screen. Loads a fresh engine each time so the load time is real. Throws on failure. */
    suspend fun benchRun(backend: LlmBackend, prompt: String, maxTokens: Int = 2048): LlmRun = lock.withLock {
        val path = modelPath() ?: throw IllegalStateException(Errors.LLM_MODEL)
        withContext(Dispatchers.Default) {
            release()
            ExperimentalFlags.enableBenchmark = true
            val t0 = System.nanoTime()
            val e = Engine(EngineConfig(modelPath = path, backend = backendObj(backend), maxNumTokens = maxTokens, cacheDir = ctx.cacheDir.absolutePath))
            try {
                e.initialize()
                val load = (System.nanoTime() - t0) / 1e9
                e.createConversation().use { c ->
                    c.sendMessage(prompt)
                    val b = c.getBenchmarkInfo()
                    val ram = Debug.MemoryInfo().also { Debug.getMemoryInfo(it) }.totalPss / 1024.0
                    LlmRun(load, b.timeToFirstTokenInSecond, b.lastPrefillTokensPerSecond, b.lastDecodeTokensPerSecond, b.lastPrefillTokenCount, b.lastDecodeTokenCount, ram)
                }
            } finally {
                runCatching { e.close() }
            }
        }
    }
}

class LlmTranslator(private val llm: LlmRuntime) : Translator {
    override suspend fun translate(text: String, from: String, to: String): String {
        if (from == to) return text
        val p = Prompts.translate(text, Languages.name(from), Languages.name(to))
        return Prompts.cleanTranslation(llm.generate(p.system, p.user))
    }
}

/** Summaries from the on-device model. Falls back to the simple summarizer if the model cannot run. */
class LlmSummarizer(private val llm: LlmRuntime, private val fallback: Summarizer = ExtractiveSummarizer()) : Summarizer {
    override val name = "On-device AI (Gemma)"
    @Volatile var lastError: String? = null

    override suspend fun summarize(transcript: String): Summary {
        lastError = null
        return try {
            val p = Prompts.summarize(transcript)
            LlmFormat.parse(llm.generate(p.system, p.user), name) ?: throw IllegalStateException("unreadable reply")
        } catch (e: Throwable) {
            lastError = Errors.LLM_FAIL
            fallback.summarize(transcript)
        }
    }
}

// ------------------------------------------------------------------ Find phone + music keys

class Ringer(private val ctx: Context) {
    private var ringtone: Ringtone? = null
    private var oldVolume = -1

    val ringing get() = ringtone != null

    fun start() {
        if (ringtone != null) return
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        oldVolume = am.getStreamVolume(AudioManager.STREAM_ALARM)
        am.setStreamVolume(AudioManager.STREAM_ALARM, am.getStreamMaxVolume(AudioManager.STREAM_ALARM), 0)
        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM) ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
        ringtone = RingtoneManager.getRingtone(ctx, uri)?.apply {
            audioAttributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()
            isLooping = true
            play()
        }
    }

    fun stop() {
        ringtone?.stop(); ringtone = null
        if (oldVolume >= 0) {
            (ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager).setStreamVolume(AudioManager.STREAM_ALARM, oldVolume, 0)
            oldVolume = -1
        }
    }
}

object Media {
    /** Handles the watch Remote app: toggle / next / prev / vol_up / vol_down. */
    fun command(ctx: Context, cmd: String) {
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        fun key(code: Int) {
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
            am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
        }
        when (cmd) {
            "toggle" -> key(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)
            "next" -> key(KeyEvent.KEYCODE_MEDIA_NEXT)
            "prev" -> key(KeyEvent.KEYCODE_MEDIA_PREVIOUS)
            "vol_up" -> am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, AudioManager.FLAG_SHOW_UI)
            "vol_down" -> am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, AudioManager.FLAG_SHOW_UI)
        }
    }
}
