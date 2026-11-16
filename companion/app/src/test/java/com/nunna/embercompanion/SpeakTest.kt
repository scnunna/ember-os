package com.nunna.embercompanion

import com.nunna.embercompanion.link.*
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.*
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import kotlin.math.PI
import kotlin.math.log10
import kotlin.math.sin

class AdpcmEncoderTest {
    private fun vec(): Pair<ShortArray, List<ByteArray>> {
        val j = Json.parseToJsonElement(javaClass.getResourceAsStream("/link_vectors.json")!!.readBytes().decodeToString()).jsonObject
        val a = j["adpcm"]!!.jsonObject
        val input = ShortArray(512) { a["input"]!!.jsonArray[it].jsonPrimitive.int.toShort() }
        val blocks = a["blocks_hex"]!!.jsonArray.map { e ->
            val h = e.jsonPrimitive.content
            ByteArray(h.length / 2) { h.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
        }
        return input to blocks
    }

    private fun snrDb(a: ShortArray, b: ShortArray): Double {
        var sig = 0.0; var err = 0.0
        for (i in a.indices) { sig += a[i].toDouble() * a[i]; val d = (a[i] - b[i]).toDouble(); err += d * d }
        return 10 * log10(sig / maxOf(err, 1e-9))
    }

    private fun decodeAll(blocks: List<ByteArray>) = blocks.flatMap { ImaAdpcm.decodeBlock(it)!!.toList() }.toShortArray()

    @Test fun matchesFirmwareVectorsBitExact() {
        val (input, expected) = vec()
        val got = ImaAdpcmEncoder().encodeAll(input)
        assertEquals(2, got.size)
        for (i in 0..1) assertArrayEquals("block $i", expected[i], got[i])
    }

    @Test fun blockLayout() {
        val pcm = ShortArray(256) { (3000 * sin(2 * PI * 300 * it / 16000)).toInt().toShort() }
        val b = ImaAdpcmEncoder().encodeBlock(pcm)
        assertEquals(133, b.size)
        assertEquals(0, b[0].toInt()); assertEquals(0, b[3].toInt()); assertEquals(0, b[4].toInt())
        assertEquals(pcm[0].toInt(), (b[1].toInt() and 0xFF) or (b[2].toInt() shl 8))
    }

    @Test fun sineRoundTripAndBlockBoundaries() {
        val pcm = ShortArray(256 * 12) { (9000 * sin(2 * PI * 440 * it / 16000) + 3000 * sin(2 * PI * 1250 * it / 16000)).toInt().toShort() }
        val blocks = ImaAdpcmEncoder().encodeAll(pcm)
        assertEquals(12, blocks.size)
        assertEquals((0 until 12).toList(), blocks.map { ImaAdpcm.seqOf(it) })
        val dec = decodeAll(blocks)
        assertTrue("snr " + snrDb(pcm, dec), snrDb(pcm, dec) > 20)
        for (k in 1 until 12) assertTrue(Math.abs(dec[k * 256 - 1] - pcm[k * 256 - 1]) < 1500)
    }

    @Test fun speechLikeSignalRoundTrip() {
        val rnd = java.util.Random(7)
        val n = 16000 * 2
        val pcm = ShortArray(n) { i ->
            val t = i / 16000.0
            val env = maxOf(0.0, sin(2 * PI * 3 * t)) * (0.5 + 0.5 * sin(2 * PI * 0.7 * t))
            val f0 = 120 + 30 * sin(2 * PI * 1.5 * t)
            var v = 0.0
            for (h in 1..10) v += sin(2 * PI * f0 * h * t) / h
            (env * 7000 * v + rnd.nextGaussian() * 40).toInt().coerceIn(-32768, 32767).toShort()
        }
        val blocks = ImaAdpcmEncoder().encodeAll(pcm)
        val dec = decodeAll(blocks)
        assertEquals(blocks.size * 256, dec.size)
        val snr = snrDb(pcm, dec.copyOf(n))
        assertTrue("snr $snr", snr > 20)
    }

    @Test fun lastBlockPaddedAndSeqWraps() {
        val pcm = ShortArray(256 * 260 + 5) { (1000 * sin(it / 9.0)).toInt().toShort() }
        val blocks = ImaAdpcmEncoder().encodeAll(pcm)
        assertEquals(261, blocks.size)
        assertEquals(4, ImaAdpcm.seqOf(blocks[260]))
        assertEquals(0, ImaAdpcm.seqOf(blocks[256]))
        assertTrue(blocks.all { it.size == 133 })
    }

    @Test fun stepIndexCarriesOver() {
        val loud = ShortArray(512) { if (it % 2 == 0) 20000 else (-20000).toShort() }
        val b = ImaAdpcmEncoder().encodeAll(loud)
        assertEquals(0, b[0][3].toInt())
        assertTrue("index carried into block 2: " + b[1][3], b[1][3].toInt() > 20)
    }
}

class SpeakMessagesTest {
    private val all = listOf<Message>(
        Message.Say("abc-1", 188), Message.SayEnd("abc-1"), Message.SayStop("test"), Message.SpkCfg(70),
        Message.SayReady("abc-1"), Message.SaySkip("abc-1", "recording"), Message.SayDone("abc-1", 188, 2),
    )

    @Test fun roundTrip() { for (m in all) assertEquals(m, MessageCodec.decode(MessageCodec.encode(m))) }

    @Test fun jsonShape() {
        assertEquals("{\"t\":\"say\",\"id\":\"x\",\"n\":5}", MessageCodec.encode(Message.Say("x", 5)))
        assertEquals("{\"t\":\"spk_cfg\",\"vol\":40}", MessageCodec.encode(Message.SpkCfg(40)))
        assertEquals("{\"t\":\"say_done\",\"id\":\"x\",\"played\":4,\"lost\":1}", MessageCodec.encode(Message.SayDone("x", 4, 1)))
    }

    @Test fun throughFramingAtMtu23() {
        for (m in all) {
            val re = Reassembler()
            var out: ByteArray? = null
            for (p in Framing.split(MessageCodec.encodeBytes(m), 23)) { assertTrue(p.size <= 20); re.feed(p)?.let { out = it } }
            assertEquals(m, MessageCodec.decodeBytes(out!!))
        }
    }
}

class SpeechPrepTest {
    @Test fun cleanText() {
        assertEquals("Call mum. Buy milk. Gym at 6.", SpeechPrep.cleanText("• Call mum\n• Buy milk\n\nGym at 6."))
        assertEquals("One. Two", SpeechPrep.cleanText("One\n  Two  "))
        assertEquals("", SpeechPrep.cleanText(" \n • "))
    }

    @Test fun resampleAndLevel() {
        val pcm = ShortArray(22050) { (10000 * sin(2 * PI * 300 * it / 22050)).toInt().toShort() }
        val out = SpeechPrep.to16k(pcm, 22050)
        assertEquals(16000.0, out.size.toDouble(), 2.0)
        val peak = out.maxOf { Math.abs(it.toInt()) }
        assertTrue("peak $peak", peak in 28000..30500)
        assertEquals(0, SpeechPrep.to16k(ShortArray(0), 22050).size)
    }
}

class VoicePickTest {
    private val v = listOf(
        VoiceInfo("en-gb-x-gba-local", "en-GB", 400, 200, false),
        VoiceInfo("en-us-x-iom-local", "en-US", 300, 200, false),
        VoiceInfo("en-us-x-tpd-local", "en-US", 400, 200, false),
        VoiceInfo("en-us-x-tpd-network", "en-US", 500, 200, true),
        VoiceInfo("en-us-x-xx-local", "en-US", 500, 200, false, notInstalled = true),
        VoiceInfo("fr-fr-x-vlf-local", "fr-FR", 500, 200, false),
    )

    @Test fun onlyOfflineInstalledEnglish() {
        assertEquals(listOf("en-us-x-tpd-local", "en-us-x-iom-local", "en-gb-x-gba-local"), VoicePick.sorted(v).map { it.name })
    }

    @Test fun defaultAndSaved() {
        assertEquals("en-us-x-tpd-local", VoicePick.choose(v, null)!!.name)
        assertEquals("en-gb-x-gba-local", VoicePick.choose(v, "en-gb-x-gba-local")!!.name)
        assertEquals("en-us-x-tpd-local", VoicePick.choose(v, "en-us-x-tpd-network")!!.name)
        assertNull(VoicePick.choose(emptyList(), null))
    }

    @Test fun friendly() {
        val f = VoicePick.friendlyNames(v)
        assertEquals("English (United States), voice 1, high quality", f["en-us-x-tpd-local"])
        assertEquals("English (United States), voice 2, normal quality", f["en-us-x-iom-local"])
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
class SpeakFlowTest {
    private class Fake(val scope: TestScope) : SpeakIo {
        var avail = true
        var readyDelay: Long? = 40
        var skipWhy: String? = null
        var doneDelay = 800L
        var accept = true
        var queuedNow = 0
        lateinit var flow: SpeakFlow
        val msgs = ArrayList<Message>()
        val writes = ArrayList<Long>()
        val fast = ArrayList<Boolean>()
        val blocks = ArrayList<ByteArray>()
        override fun available() = avail
        override fun sendMsg(m: Message) {
            msgs += m
            when (m) {
                is Message.Say -> readyDelay?.let { d -> scope.launch { delay(d); flow.onMessage(skipWhy?.let { Message.SaySkip(m.id, it) } ?: Message.SayReady(m.id)) } }
                is Message.SayEnd -> scope.launch { delay(doneDelay); flow.onMessage(Message.SayDone(m.id, blocks.size, 0)) }
                else -> {}
            }
        }
        override fun writeBlock(b: ByteArray): Boolean { if (!accept) return false; writes += scope.testScheduler.currentTime; blocks += b; return true }
        override fun queued() = queuedNow
        override fun fastLink(on: Boolean) { fast += on }
        fun types() = msgs.map { it::class.simpleName }
    }

    private fun TestScope.setup(seconds: Double = 3.0, enabled: Boolean = true): Pair<SpeakFlow, Fake> {
        val io = Fake(this)
        val flow = SpeakFlow(this, io,
            synth = { ShortArray((16000 * seconds).toInt()) { (4000 * sin(it / 20.0)).toInt().toShort() } },
            enabled = { enabled }, nowMs = { testScheduler.currentTime }, sleep = { delay(it) })
        io.flow = flow
        return flow to io
    }

    @Test fun readyStreamEndDone() = runTest {
        val (flow, io) = setup()
        flow.enqueue("m1", "• Hello there\n• Gym at six")
        advanceUntilIdle()
        val say = io.msgs[0] as Message.Say
        assertEquals("m1", say.id)
        assertEquals(188, say.n)
        assertEquals(188, io.blocks.size)
        assertEquals(listOf("Say", "SayEnd"), io.types())
        assertEquals(listOf(true, false), io.fast)
        assertEquals((0 until 188).map { it and 0xFF }, io.blocks.map { ImaAdpcm.seqOf(it) })
        assertFalse(flow.busy)
    }

    @Test fun pacingNeverMoreThanAboutASecondAhead() = runTest {
        val (flow, io) = setup()
        flow.enqueue("m1", "Hello")
        advanceUntilIdle()
        val t0 = 40L
        for ((i, t) in io.writes.withIndex()) {
            val elapsed = t - t0
            assertTrue("block $i at $elapsed ms", i <= elapsed / 16 + 63)
            assertTrue("block $i too early at $elapsed", elapsed >= i * 16L - 1000 - 1)
        }
        assertEquals(63, io.writes.count { it - t0 == 0L })
        assertTrue(io.writes.last() - t0 >= (187 * 16 - 1000 - 1))
    }

    @Test fun skipSendsNothingMore() = runTest {
        val (flow, io) = setup()
        io.skipWhy = "recording"
        flow.enqueue("m1", "Hello")
        advanceUntilIdle()
        assertEquals(listOf("Say"), io.types()); assertTrue(io.blocks.isEmpty()); assertTrue(io.fast.isEmpty())
    }

    @Test fun noReadyWithinTwoSecondsGivesUp() = runTest {
        val (flow, io) = setup()
        io.readyDelay = null
        flow.enqueue("m1", "Hello")
        advanceUntilIdle()
        assertEquals(2000L, testScheduler.currentTime)
        assertEquals(listOf("Say", "SayStop"), io.types()); assertTrue(io.blocks.isEmpty())
        assertFalse(flow.busy)
    }

    @Test fun watchStopsMidStream() = runTest {
        val (flow, io) = setup(seconds = 10.0)
        flow.enqueue("m1", "Hello")
        advanceTimeBy(1500); runCurrent()
        val n = io.blocks.size
        assertTrue("n=$n", n in 60..160)
        flow.onMessage(Message.SayStop("m1"))
        advanceUntilIdle()
        assertEquals(n, io.blocks.size)
        assertEquals(listOf("Say"), io.types())
        assertEquals(listOf(true, false), io.fast)
    }

    @Test fun phoneStopsMidStream() = runTest {
        val (flow, io) = setup(seconds = 10.0)
        flow.enqueue("m1", "Hello")
        advanceTimeBy(1500); runCurrent()
        val n = io.blocks.size
        flow.stop()
        advanceUntilIdle()
        assertEquals(n, io.blocks.size)
        assertEquals(listOf("Say", "SayStop"), io.types())
        assertEquals(Message.SayStop("m1"), io.msgs.last())
    }

    @Test fun disconnectMidStream() = runTest {
        val (flow, io) = setup(seconds = 10.0)
        flow.enqueue("m1", "Hello"); flow.enqueue("m2", "Second")
        advanceTimeBy(1500); runCurrent()
        val n = io.blocks.size
        flow.onDisconnected()
        advanceUntilIdle()
        assertEquals(n, io.blocks.size)
        assertEquals(listOf("Say"), io.types())
        assertEquals(listOf(true, false), io.fast)
        assertFalse(flow.busy)
    }

    @Test fun queueKeepsOnlyTheNewestWaiting() = runTest {
        val (flow, io) = setup(seconds = 1.0)
        flow.enqueue("a", "First")
        advanceTimeBy(100); runCurrent()          // "a" is now speaking
        flow.enqueue("b", "Second"); flow.enqueue("c", "Third")
        advanceUntilIdle()
        assertEquals(listOf("a", "c"), io.msgs.filterIsInstance<Message.Say>().map { it.id })
        assertEquals(2, io.msgs.filterIsInstance<Message.SayEnd>().size)
    }

    @Test fun offSettingAndUnavailableAreSilent() = runTest {
        val (off, io1) = setup(enabled = false)
        off.enqueue("m", "Hello"); advanceUntilIdle()
        assertTrue(io1.msgs.isEmpty())
        off.enqueue("test", "Hello", force = true); advanceUntilIdle()
        assertEquals("test", (io1.msgs.first() as Message.Say).id)
        val (flow, io2) = setup()
        io2.avail = false
        flow.enqueue("m", "Hello"); advanceUntilIdle()
        assertTrue(io2.msgs.isEmpty())
    }

    @Test fun backpressureWaitsForTheWriteQueue() = runTest {
        val (flow, io) = setup(seconds = 3.0)
        io.queuedNow = 100
        flow.enqueue("m1", "Hello")
        advanceTimeBy(500); runCurrent()
        assertTrue(io.blocks.isEmpty())
        io.queuedNow = 0
        advanceUntilIdle()
        assertEquals(188, io.blocks.size)
    }

    @Test fun wrongIdIsIgnored() = runTest {
        val (flow, io) = setup(seconds = 1.0)
        io.readyDelay = null
        flow.enqueue("m1", "Hello")
        advanceTimeBy(100); runCurrent()
        flow.onMessage(Message.SayReady("other"))
        advanceUntilIdle()
        assertEquals(listOf("Say", "SayStop"), io.types())
    }
}
