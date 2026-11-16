package com.nunna.embercompanion.link

import com.nunna.embercompanion.protocol.ImaAdpcm
import com.nunna.embercompanion.protocol.ImaAdpcmEncoder
import com.nunna.embercompanion.protocol.Message
import com.nunna.embercompanion.protocol.Resampler
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/** Text and audio preparation for speech (pure Kotlin). */
object SpeechPrep {
    /** Strips leading bullets and joins lines with ". " so the voice pauses between items. */
    fun cleanText(raw: String): String {
        val parts = raw.lines().map { it.trim().trimStart('\u2022', '-', '*', '\u00b7').trim() }.filter { it.isNotEmpty() }
        val sb = StringBuilder()
        for ((i, p) in parts.withIndex()) {
            if (i > 0) sb.append(if (sb.last() in ".!?:;") " " else ". ")
            sb.append(p)
        }
        return sb.toString().take(600)
    }

    /** Any-rate PCM to 16 kHz mono 16-bit, with the level lifted to about 90% of full scale (max gain x4) for the small speaker. */
    fun to16k(pcm: ShortArray, rate: Int): ShortArray {
        val f = Resampler.toFloat(pcm, rate, 16000)
        var peak = 0f
        for (v in f) peak = maxOf(peak, Math.abs(v))
        val gain = if (peak < 0.01f) 1f else (0.9f / peak).coerceIn(0.25f, 4f)
        return ShortArray(f.size) { (f[it] * gain * 32767f).toInt().coerceIn(-32768, 32767).toShort() }
    }
}

/** What [SpeakFlow] needs from the Bluetooth side. */
interface SpeakIo {
    /** True when the watch is connected and has the SPK characteristic (old firmware: false, speaking is silently unavailable). */
    fun available(): Boolean
    fun sendMsg(m: Message)
    /** Queues one 133-byte block on SPK (Write Without Response). False = could not queue. */
    fun writeBlock(b: ByteArray): Boolean
    /** Blocks queued on SPK but not yet written. */
    fun queued(): Int
    /** Fast link while streaming, balanced afterwards. */
    fun fastLink(on: Boolean)
}

/**
 * Amber speaks on the watch (EMBER_LINK v1.5). One utterance at a time; at most one waits and a newer one replaces it.
 * Flow: say -> say_ready (2 s) -> blocks at 62.5/s with up to 1 s lead -> say_end -> say_done.
 * [synth] turns text into 16 kHz mono PCM (null = no audio) and runs off the main thread in the real app.
 */
class SpeakFlow(
    private val scope: CoroutineScope,
    private val io: SpeakIo,
    private val synth: suspend (String) -> ShortArray?,
    private val enabled: () -> Boolean,
    private val nowMs: () -> Long,
    private val sleep: suspend (Long) -> Unit,
) {
    companion object {
        const val BLOCK_MS = 16L              // 256 samples at 16 kHz
        const val LEAD_MS = 1000L
        const val READY_TIMEOUT_MS = 2000L
        const val DONE_TIMEOUT_MS = 5000L
        const val MAX_QUEUED_WRITES = 24
        private const val READY = "ready"
    }

    private class Item(val id: String, val text: String)

    private val lock = Any()
    private var pending: Item? = null
    private var job: Job? = null
    private var activeId: String? = null
    private var ready: CompletableDeferred<String>? = null   // "ready", else the say_skip reason / stop / disconnected
    private var done: CompletableDeferred<Unit>? = null
    @Volatile private var abort = false

    /** Speaks [text] (an Amber reply with final=true, or the test sentence with [force]). */
    fun enqueue(id: String, text: String, force: Boolean = false) {
        if (!force && !enabled()) return
        val clean = SpeechPrep.cleanText(text)
        if (clean.isEmpty()) return
        synchronized(lock) {
            pending = Item(id, clean)
            if (job?.isActive == true) return
            job = scope.launch { drain() }
        }
    }

    /** Watch events: say_ready, say_skip, say_done, say_stop. Others are ignored. */
    fun onMessage(m: Message) {
        synchronized(lock) {
            when (m) {
                is Message.SayReady -> if (m.id == activeId) ready?.complete(READY)
                is Message.SaySkip -> if (m.id == activeId) ready?.complete(m.why.ifEmpty { "skip" })
                is Message.SayDone -> if (m.id == activeId) { done?.complete(Unit); abort = true }
                is Message.SayStop -> if (m.id == activeId) { abort = true; done?.complete(Unit); ready?.complete("stop") }
                else -> {}
            }
        }
    }

    /** The link dropped: everything stops, nothing is sent. */
    fun onDisconnected() {
        synchronized(lock) {
            pending = null
            abort = true
            ready?.complete("disconnected"); done?.complete(Unit)
        }
    }

    /** User stop: tells the watch and drops the queue. */
    fun stop() {
        val id: String?
        synchronized(lock) { pending = null; abort = true; id = activeId; ready?.complete("stop"); done?.complete(Unit) }
        if (id != null) io.sendMsg(Message.SayStop(id))
    }

    val busy: Boolean get() = synchronized(lock) { job?.isActive == true }

    private suspend fun drain() {
        while (true) {
            val item = synchronized(lock) { pending.also { pending = null } } ?: break
            try { speakOne(item) } catch (e: kotlinx.coroutines.CancellationException) { throw e } catch (e: Exception) { /* quiet */ }
        }
        synchronized(lock) { job = null }
    }

    private suspend fun speakOne(item: Item) {
        if (!io.available()) return
        val pcm = synth(item.text) ?: return
        if (pcm.isEmpty() || !io.available()) return
        val blocks = ImaAdpcmEncoder().encodeAll(pcm)
        val r = CompletableDeferred<String>()
        val d = CompletableDeferred<Unit>()
        synchronized(lock) { abort = false; activeId = item.id; ready = r; done = d }
        var streamed = false
        try {
            io.sendMsg(Message.Say(item.id, blocks.size))
            val why = withTimeoutOrNull(READY_TIMEOUT_MS) { r.await() }
            if (why == null) { io.sendMsg(Message.SayStop(item.id)); return }   // 2 s without say_ready: give up quietly (tell the watch to close)
            if (why != READY) return                                           // skipped, stopped or disconnected
            io.fastLink(true); streamed = true
            val start = nowMs()
            for ((i, b) in blocks.withIndex()) {
                if (abort) return
                val due = start + i * BLOCK_MS - LEAD_MS
                val wait = due - nowMs()
                if (wait > 0) sleep(wait)
                if (abort) return
                while (io.queued() > MAX_QUEUED_WRITES && !abort) sleep(5)
                if (abort) return
                if (!io.writeBlock(b)) return
            }
            if (abort) return
            io.sendMsg(Message.SayEnd(item.id))
            withTimeoutOrNull(DONE_TIMEOUT_MS) { d.await() }
        } finally {
            if (streamed) io.fastLink(false)
            synchronized(lock) { activeId = null; ready = null; done = null }
        }
    }
}
