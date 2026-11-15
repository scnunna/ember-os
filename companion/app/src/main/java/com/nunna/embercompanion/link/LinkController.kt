package com.nunna.embercompanion.link

import com.nunna.embercompanion.protocol.AdpcmStream
import com.nunna.embercompanion.protocol.Framing
import com.nunna.embercompanion.protocol.Message
import com.nunna.embercompanion.protocol.MessageCodec
import com.nunna.embercompanion.protocol.Reassembler
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

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
class PttClip(val side: String, val rate: Int, private val stream: AdpcmStream, val expectedBlocks: Int = -1) {
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
        send(Message.Time(nowMs() / 1000, tzOffsetSeconds()))
        requestSteps()
        send(Message.PracticeReq)
        languages()?.let { send(Message.TrCfg(it.me, it.them)) }
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
            is Message.Hello -> _watch.value = WatchInfo(m.fw, m.name, m.batt, m.mv, m.usb, m.steps)
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
        } else {
            val side = pttSide ?: return
            pttSide = null
            onPttClip(PttClip(side, pttRate, audio, p.blocks))
        }
    }

    @Synchronized fun onAudioPacket(block: ByteArray) {
        audio.feed(block)
    }
}
