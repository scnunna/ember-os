package com.nunna.embercompanion.link

import com.nunna.embercompanion.protocol.AdpcmStream
import com.nunna.embercompanion.protocol.Framing
import com.nunna.embercompanion.protocol.Message
import com.nunna.embercompanion.protocol.MessageCodec
import com.nunna.embercompanion.protocol.Reassembler
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** The bottom of the stack: something that can write already-framed packets to the watch's CMD characteristic. */
interface LinkTransport {
    /** Queue these packets for sending, in order. */
    fun write(packets: List<ByteArray>)
}

enum class LinkPhase { DISCONNECTED, CONNECTING, CONNECTED }

data class WatchInfo(val fw: String, val name: String, val batt: Int, val mv: Int, val usb: Boolean, val steps: Int)

/** Languages for the translate app, sent as tr_cfg on connect. */
data class TrLanguages(val me: String, val them: String)

/** A finished push-to-talk recording from the watch. */
class PttClip(val side: String, val rate: Int, private val stream: AdpcmStream, val expectedBlocks: Int = -1, val ctx: Message.PttCtx? = null) {
    /** True when every block the watch said it sent has arrived (or was counted lost). */
    val complete: Boolean get() = expectedBlocks < 0 || stream.goodBlocks >= expectedBlocks

    /** Read these after a short delay: AUDIO and EVT are separate characteristics, so the last blocks can land just after `ptt off`. */
    val samples: ShortArray get() = stream.samples()
    val lostBlocks: Int get() = stream.lostBlocks
}

/**
 * Pure-Kotlin link state machine. Android glue (BLE) calls [onConnecting], [onReady], [onEvtPacket], [onAudioPacket],
 * [onDisconnected]; everything the app wants to say to the watch goes through [send]. No Android classes here so it
 * can be tested against a fake watch.
 */
class LinkController(
    private val transport: LinkTransport,
    private val nowMs: () -> Long,
    private val tzOffsetSeconds: () -> Int,
    private val languages: () -> TrLanguages?,
    private val phoneName: () -> String? = { null },   // v1.2: shown on the watch's Phone screen
    // v1.4 life manager (all optional: without them the link behaves exactly as before)
    private val lifeOs: LifeOsApi? = null,
    private val checkinLog: CheckinLog? = null,
    private val ciConfig: () -> Message.CiCfg? = { null },
) {
    private val _phase = MutableStateFlow(LinkPhase.DISCONNECTED)
    val phase: StateFlow<LinkPhase> = _phase.asStateFlow()
    private val _watch = MutableStateFlow<WatchInfo?>(null)
    val watch: StateFlow<WatchInfo?> = _watch.asStateFlow()
    private val _mtu = MutableStateFlow(Framing.DEFAULT_MTU)
    val mtu: StateFlow<Int> = _mtu.asStateFlow()

    /** Called for every message from the watch (after the controller has handled hello). */
    var onMessage: (Message) -> Unit = {}
    /** Called when the watch ends a push-to-talk (ptt off) and we have the decoded audio. */
    var onPttClip: (PttClip) -> Unit = {}
    /** Called when a connection is ready (after the on-connect messages were queued). */
    var onReadyHook: () -> Unit = {}

    private val reassembler = Reassembler()
    private var lastStepsReq = 0L
    private var pttSide: String? = null
    private var pttRate = 16000
    private var pttCtx: Message.PttCtx? = null
    private var audio = AdpcmStream()

    val hourMs = 3_600_000L

    @Synchronized fun onConnecting() { _phase.value = LinkPhase.CONNECTING }

    /**
     * GATT connected, MTU negotiated, EVT and AUDIO subscribed. [secure] must be true only when the link is bonded
     * (LE Secure Connections + passkey) and encrypted; otherwise nothing is sent at all.
     */
    @Synchronized fun onReady(mtu: Int, secure: Boolean = true) {
        if (!secure) { _phase.value = LinkPhase.CONNECTING; return }
        _mtu.value = mtu
        _phase.value = LinkPhase.CONNECTED
        pttSide = null
        send(Message.HelloReq)                 // the watch's own hello can arrive before we listen (restored CCCD)
        phoneName()?.takeIf { it.isNotBlank() }?.let { send(Message.PhoneInfo(it.take(31))) }
        send(Message.Time(nowMs() / 1000, tzOffsetSeconds()))
        requestSteps()
        send(Message.PracticeReq)
        languages()?.let { send(Message.TrCfg(it.me, it.them)) }
        ciConfig()?.let { send(it) }
        onReadyHook()
    }

    @Synchronized fun onDisconnected() {
        _phase.value = LinkPhase.DISCONNECTED
        pttSide = null
    }

    @Synchronized fun requestSteps() {
        lastStepsReq = nowMs()
        send(Message.StepsReq)
    }

    /** Call about once a minute; asks for steps again when an hour has passed. */
    @Synchronized fun tick() {
        if (_phase.value == LinkPhase.CONNECTED && nowMs() - lastStepsReq >= hourMs) requestSteps()
    }

    @Synchronized fun send(m: Message) {
        if (_phase.value != LinkPhase.CONNECTED) return
        transport.write(Framing.split(MessageCodec.encodeBytes(m), _mtu.value))
    }

    @Synchronized fun onEvtPacket(packet: ByteArray) {
        val bytes = reassembler.feed(packet) ?: return
        val m = MessageCodec.decodeBytes(bytes) ?: return
        when (m) {
            is Message.Hello -> {
                _watch.value = WatchInfo(m.fw, m.name, m.batt, m.mv, m.usb, m.steps)
                watchSd = m.sd; planDue = true; nextDrainMs = 0; drainBackoffMs = 0
            }
            is Message.SchedOk -> watchSd = m.date
            is Message.Ci -> handleCi(m)
            is Message.Ptt -> handlePtt(m)
            else -> {}
        }
        onMessage(m)
    }

    private fun handlePtt(p: Message.Ptt) {
        if (p.on) {
            audio = AdpcmStream()
            pttSide = p.side
            pttRate = p.rate
            pttCtx = p.ctx
        } else {
            val side = pttSide ?: return
            pttSide = null
            onPttClip(PttClip(side, pttRate, audio, p.blocks, p.ctx ?: pttCtx))
        }
    }

    @Synchronized fun onAudioPacket(block: ByteArray) {
        audio.feed(block)
    }

    // ---------------------------------------------------------------- v1.4 life manager

    private val lifeMutex = Mutex()
    private val _plan = MutableStateFlow<Plan?>(null)
    /** The latest plan fetched from Life OS (for the Home "Now" line). */
    val plan: StateFlow<Plan?> = _plan.asStateFlow()
    private val _lifeError = MutableStateFlow<String?>(null)
    /** Last Life OS problem as a Contract code (NOT_CONFIGURED, OFFLINE, HTTP_ERROR...), or null when fine. */
    val lifeError: StateFlow<String?> = _lifeError.asStateFlow()

    private var watchSd = 0
    private var planDue = false
    private var sentGen: String? = null
    private var sentDate = 0
    private var nextPlanMs = 0L
    private var lastFetchDate = 0
    private var nextDrainMs = 0L
    private var drainBackoffMs = 0L

    val planIntervalMs = 15 * 60_000L

    /** Today's adherence from the local check-in log, or null when no log is wired. */
    fun adherence(): Adherence? = checkinLog?.let { LifeMath.adherence(it.all.value, nowMs(), tzOffsetSeconds()) }
    fun nowLine(): String? = LifeMath.nowLine(_plan.value, nowMs(), tzOffsetSeconds())
    fun pendingCheckins(): Int = checkinLog?.pendingCount() ?: 0

    /** Stores first, then acknowledges: a crash between the two only means the watch re-sends. */
    private fun handleCi(m: Message.Ci) {
        val log = checkinLog ?: return
        val valid = m.slot in 0..1440 && LifeMath.isoToInt(
            "%04d-%02d-%02d".format(m.date / 10000, m.date / 100 % 100, m.date % 100),
        ) == m.date && m.ans in setOf("on", "off", "snooze", "missed") && m.at > 0
        if (valid) {
            try { log.append(Checkin(m.seq, m.date, m.slot, m.at, m.ans, m.ids.filter { it.length in 1..36 }.take(24))) } catch (e: Exception) { return } // could not save: no ack, the watch keeps it
        }
        send(Message.CiAck(m.seq))
    }

    /**
     * Call about once a minute while the service runs, and right after a hello or a `ci`. Safe to call often.
     * Connected: fetch today's plan from Life OS (after hello, every 15 min, and after local midnight) and push `sched`
     * to the watch when `gen` changed or the watch's stored date is not the plan's date. Connected or not: forward
     * queued check-ins to Life OS (batches of 64) with retry and backoff.
     */
    suspend fun lifeTick() {
        val api = lifeOs ?: return
        lifeMutex.withLock {
            val now = nowMs()
            if (_phase.value == LinkPhase.CONNECTED) {
                val today = LifeMath.localDateInt(now, tzOffsetSeconds())
                val due = planDue || now >= nextPlanMs || (lastFetchDate != 0 && lastFetchDate != today)
                if (due) { planDue = false; syncPlan(api, now, today) }
            }
            drain(api, now)
        }
    }

    private suspend fun syncPlan(api: LifeOsApi, now: Long, today: Int) {
        val tz = tzOffsetSeconds()
        val iso = LifeMath.localDate(now, tz).toString()
        val r = api.today(iso, Math.floorDiv(tz, 60))
        if (!r.ok) { _lifeError.value = r.code; nextPlanMs = now + 60_000; return }
        val p = LifeMath.parsePlan(r.body)
        if (p == null) { _lifeError.value = "BAD_REPLY"; nextPlanMs = now + 60_000; return }
        _lifeError.value = null
        nextPlanMs = now + planIntervalMs
        lastFetchDate = today
        _plan.value = p
        synchronized(this) {
            if (p.gen != sentGen || p.date != sentDate || watchSd != p.date) {
                sentGen = p.gen; sentDate = p.date
                send(Message.Sched(p.date, p.gen, p.slots, p.blocks))
            }
        }
    }

    private suspend fun drain(api: LifeOsApi, now: Long) {
        val log = checkinLog ?: return
        if (now < nextDrainMs) return
        while (true) {
            val batch = log.unsent(64)
            if (batch.isEmpty()) { drainBackoffMs = 0; break }
            val r = api.checkins(LifeMath.checkinBody(batch))
            val done = if (r.ok) batch.filter { r.upto < 0 || it.seq <= r.upto } else emptyList()
            if (done.isEmpty()) {
                if (!r.ok) _lifeError.value = r.code
                drainBackoffMs = if (drainBackoffMs == 0L) 30_000 else minOf(drainBackoffMs * 2, 15 * 60_000L)
                nextDrainMs = now + drainBackoffMs
                break
            }
            log.markSent(done.map { it.key }.toSet())
            _lifeError.value = null
        }
    }
}
