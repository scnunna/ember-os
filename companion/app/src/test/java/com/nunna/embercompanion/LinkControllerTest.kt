package com.nunna.embercompanion

import com.nunna.embercompanion.link.*
import com.nunna.embercompanion.protocol.*
import org.junit.Assert.*
import org.junit.Test

/** In-memory watch: reassembles what the phone writes, and can push EVT / AUDIO packets back at the controller. */
class FakeWatch(private val mtu: Int = 247) : LinkTransport {
    val received = ArrayList<Message>()
    private val re = Reassembler()
    lateinit var link: LinkController

    override fun write(packets: List<ByteArray>) {
        for (p in packets) {
            assertTrue("packet within MTU-3", p.size <= mtu - 3)
            re.feed(p)?.let { bytes -> MessageCodec.decodeBytes(bytes)?.let { received += it } }
        }
    }

    fun say(m: Message) {
        for (p in Framing.split(MessageCodec.encodeBytes(m), mtu)) link.onEvtPacket(p)
    }

    fun audio(block: ByteArray) = link.onAudioPacket(block)

    fun types() = received.map { it::class.simpleName }
}

class LinkControllerTest {
    private var now = 1_763_000_000_000L
    private val watch = FakeWatch()
    private val link = LinkController(watch, { now }, { -18000 }, { TrLanguages("en", "fr") }).also { watch.link = it }

    @Test fun connectFlow() {
        assertEquals(LinkPhase.DISCONNECTED, link.phase.value)
        link.onConnecting()
        assertEquals(LinkPhase.CONNECTING, link.phase.value)
        link.onReady(247)
        assertEquals(LinkPhase.CONNECTED, link.phase.value)
        watch.say(Message.Hello("1.0", "Watch-ABCD", 80, 4000, false, 500))
        assertEquals(80, link.watch.value!!.batt)
        // time first, then steps_req, practice_req, tr_cfg
        assertEquals(listOf("Time", "StepsReq", "PracticeReq", "TrCfg"), watch.types())
        val t = watch.received[0] as Message.Time
        assertEquals(1_763_000_000L, t.epoch)
        assertEquals(-18000, t.tz)
        assertEquals(Message.TrCfg("en", "fr"), watch.received[3])
    }

    @Test fun nothingIsSentBeforeTheLinkIsSecure() {
        link.onConnecting()
        link.onReady(247, secure = false) // not bonded yet
        assertEquals(LinkPhase.CONNECTING, link.phase.value)
        link.send(Message.FindWatch)
        assertTrue(watch.received.isEmpty())
        link.onReady(247, secure = true)
        assertEquals(LinkPhase.CONNECTED, link.phase.value)
        assertEquals("Time", watch.types().first())
    }

    @Test fun pttOffCarriesBlockCount() {
        val b = javaClass.getResourceAsStream("/link_vectors.json")!!.readBytes().decodeToString()
        val hex = Regex("\"([0-9a-f]{266})\"").findAll(b).map { it.groupValues[1] }.toList()
        val blocks = hex.map { h -> ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() } }
        var clip: PttClip? = null
        link.onPttClip = { clip = it }
        link.onReady(247)
        watch.say(Message.Ptt("me", true, 16000, "ima-adpcm"))
        watch.audio(blocks[0])
        watch.say(Message.Ptt("me", false, 16000, "ima-adpcm", 2))
        assertFalse(clip!!.complete) // one block still missing
        watch.audio(blocks[1])
        assertTrue(clip!!.complete)
        assertEquals(2, clip!!.expectedBlocks)
    }

    @Test fun hourlyStepsRequest() {
        link.onReady(247)
        watch.received.clear()
        now += 30 * 60_000; link.tick()
        assertTrue(watch.received.isEmpty())
        now += 31 * 60_000; link.tick()
        assertEquals(listOf(Message.StepsReq), watch.received)
        link.tick()
        assertEquals(1, watch.received.size) // not twice
    }

    @Test fun nothingSentWhileDisconnected() {
        link.send(Message.FindWatch)
        assertTrue(watch.received.isEmpty())
        link.onReady(247)
        link.onDisconnected()
        watch.received.clear()
        link.send(Message.FindWatch)
        assertTrue(watch.received.isEmpty())
    }

    @Test fun smallMtuIsSplitAndReassembled() {
        val w = FakeWatch(23)
        val l = LinkController(w, { now }, { 0 }, { null }).also { w.link = it }
        l.onReady(23)
        val long = Message.Note("a.wav", "T", "x".repeat(300), listOf("one", "two"))
        l.send(long)
        assertEquals(long, w.received.last())
    }

    @Test fun messagesReachListener() {
        val got = ArrayList<Message>()
        link.onMessage = { got += it }
        link.onReady(247)
        watch.say(Message.Reply(5, 1))
        watch.say(Message.FindPhone(true))
        watch.say(Message.Unknown("whatever"))
        assertEquals(listOf(Message.Reply(5, 1), Message.FindPhone(true), Message.Unknown("whatever")), got)
    }

    @Test fun pttCollectsAudioAndReportsGapsAfterOff() {
        val blocks = javaClass.getResourceAsStream("/link_vectors.json")!!.readBytes().decodeToString().let {
            kotlinx.serialization.json.Json.parseToJsonElement(it)
        }.let { j ->
            val a = (j as kotlinx.serialization.json.JsonObject)["adpcm"] as kotlinx.serialization.json.JsonObject
            (a["blocks_hex"] as kotlinx.serialization.json.JsonArray).map { e ->
                val h = (e as kotlinx.serialization.json.JsonPrimitive).content
                ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
            }
        }
        var clip: PttClip? = null
        link.onPttClip = { clip = it }
        link.onReady(247)
        watch.say(Message.Ptt("them", true, 16000, "ima-adpcm"))
        watch.audio(blocks[0])
        watch.audio(blocks[1].copyOf().also { it[0] = 3 }) // seq gap of 2
        watch.say(Message.Ptt("them", false, 16000, "ima-adpcm"))
        watch.audio(blocks[0].copyOf().also { it[0] = 4 }) // a straggler that lands just after "off"
        val c = clip!!
        assertEquals("them", c.side)
        assertEquals(2, c.lostBlocks)
        assertEquals(256 * 5, c.samples.size)
    }

    @Test fun appInstallSequenceReachesWatchInOrder() {
        link.onReady(247)
        watch.received.clear()
        AppUpload.messages("water", ByteArray(2000)).forEach { link.send(it) }
        assertEquals(listOf("AppPut", "AppData", "AppData", "AppEnd"), watch.types())
        watch.say(Message.AppOk("water"))
    }
}
