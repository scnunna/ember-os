package com.nunna.embercompanion.link

import com.nunna.embercompanion.protocol.Message
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.*

/** One line of the conversation shown on the phone. [fromAmber] false = what the owner said or typed. */
data class AmberLine(val id: String, val fromAmber: Boolean, val text: String, val atMs: Long, val error: Boolean = false)

/** What Life OS offers for the Amber conversation. The real one goes through Ember Gateway. */
interface AmberApi {
    suspend fun send(bodyJson: String): LifeOsResult
    suspend fun inbox(): LifeOsResult
    suspend fun inboxAck(bodyJson: String): LifeOsResult
}

/**
 * Talk to Amber (pure Kotlin). Voice: [heardVoice] transcribes (the caller supplies the Whisper path), tells the watch what
 * was understood, posts the text and acks "Got it." to the watch. Replies are fetched by [tick]: every 20 s for 3 minutes
 * after a message was sent, otherwise every 5 minutes, only while the watch is connected (a reply that could not reach
 * the watch is never acknowledged, so it is not lost).
 */
class AmberFlow(
    private val api: AmberApi,
    private val send: (Message) -> Unit,
    private val connected: () -> Boolean,
    private val nowMs: () -> Long,
    private val tzOffsetSeconds: () -> Int,
) {
    private val mutex = Mutex()
    private val _lines = MutableStateFlow<List<AmberLine>>(emptyList())
    val lines: StateFlow<List<AmberLine>> = _lines.asStateFlow()
    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy.asStateFlow()

    private var activeUntil = 0L
    private var nextPoll = 0L
    private var seq = 0

    companion object {
        const val FAST_MS = 20_000L
        const val ACTIVE_MS = 3 * 60_000L
        const val SLOW_MS = 5 * 60_000L
        const val NO_REPLY = "Can't reach Life OS right now. Try again in a minute."
    }

    private fun add(l: AmberLine) { _lines.value = (_lines.value + l).takeLast(60) }
    private fun cut(s: String) = LifeMath.cut(s, 300)

    /** Voice path: [text] is the transcript ("" when nothing was heard). */
    suspend fun heardVoice(text: String, ctx: Message.PttCtx?) {
        val t = text.trim()
        if (t.isEmpty()) { send(Message.Amber("local", "I did not catch that. Hold the button and try again.", true)); return }
        send(Message.AmberHeard(cut(t)))
        submit(t, ctx, voice = true)
    }

    /** Posts [text] to Life OS. Also used by the typed box on the phone. */
    suspend fun submit(text: String, ctx: Message.PttCtx? = null, voice: Boolean = false) {
        val t = text.trim().take(4000)
        if (t.isEmpty()) return
        add(AmberLine("m${++seq}", false, t, nowMs()))
        _busy.value = true
        try {
            val now = nowMs()
            val body = buildJsonObject {
                put("text", t); put("at", now / 1000)
                if (ctx != null) put("ctx", buildJsonObject {
                    put("ci_seq", ctx.ciSeq); put("slot", ctx.slot)
                    put("date", "%04d-%02d-%02d".format(ctx.date / 10000, ctx.date / 100 % 100, ctx.date % 100))
                })
            }.toString()
            val r = api.send(body)
            if (!r.ok) {
                add(AmberLine("e${++seq}", true, NO_REPLY, nowMs(), error = true))
                if (voice) send(Message.Amber("local", NO_REPLY, true))
                return
            }
            val o = runCatching { Json.parseToJsonElement(r.body).jsonObject }.getOrNull()
            val ack = o?.get("ack")?.jsonPrimitive?.contentOrNull ?: "Got it."
            val id = o?.get("id")?.jsonPrimitive?.contentOrNull ?: "m$seq"
            send(Message.Amber(id.take(36), cut(ack), false))
            activeUntil = nowMs() + ACTIVE_MS
            nextPoll = nowMs() + FAST_MS
        } finally { _busy.value = false }
    }

    /** Call every few seconds; it only does something when a poll is due. Returns how many replies were delivered. */
    suspend fun tick(): Int = mutex.withLock {
        if (!connected()) return 0
        val now = nowMs()
        if (now < nextPoll) return 0
        nextPoll = now + if (now < activeUntil) FAST_MS else SLOW_MS
        pollOnce()
    }

    /** Fetches Amber's replies, hands each to the watch, then acknowledges them to Life OS. */
    suspend fun pollOnce(): Int {
        val r = api.inbox()
        if (!r.ok) return 0
        val items = runCatching { Json.parseToJsonElement(r.body).jsonObject["items"]!!.jsonArray.map { it.jsonObject } }.getOrNull() ?: return 0
        val delivered = ArrayList<String>()
        for (it in items) {
            val id = it["id"]?.jsonPrimitive?.contentOrNull ?: continue
            val text = it["text"]?.jsonPrimitive?.contentOrNull ?: continue
            if (_lines.value.none { l -> l.fromAmber && l.id == id }) { // a repeat (ack failed earlier) is only re-acknowledged
                send(Message.Amber(id.take(36), cut(text), true))
                add(AmberLine(id, true, text, nowMs()))
            }
            delivered += id
        }
        if (delivered.isNotEmpty()) {
            val ack = buildJsonObject { put("ids", JsonArray(delivered.map { JsonPrimitive(it) })) }.toString()
            api.inboxAck(ack) // if this fails the items come again; the watch keeps the id, so it shows once
        }
        return delivered.size
    }
}
