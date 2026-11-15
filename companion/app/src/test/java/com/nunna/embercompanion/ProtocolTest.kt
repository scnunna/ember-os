package com.nunna.embercompanion

import com.nunna.embercompanion.protocol.*
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.time.Instant
import java.time.ZoneId

private fun hex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
private fun ByteArray.hex() = joinToString("") { "%02x".format(it) }

private val vectors: JsonObject by lazy {
    val text = ProtocolTest::class.java.getResourceAsStream("/link_vectors.json")!!.readBytes().decodeToString()
    Json.parseToJsonElement(text).jsonObject
}

class ProtocolTest {
    @Test fun framingMatchesVectors() {
        val f = vectors["framing"]!!.jsonObject
        val mtu = f["mtu"]!!.jsonPrimitive.int
        val msg = f["message"]!!.jsonPrimitive.content
        val expected = f["packets_hex"]!!.jsonArray.map { it.jsonPrimitive.content }
        val packets = Framing.split(msg, mtu)
        assertEquals(expected, packets.map { it.hex() })
        val r = Reassembler()
        var out: ByteArray? = null
        for (p in packets) out = r.feed(p) ?: out
        assertEquals(msg, String(out!!, Charsets.UTF_8))
        // and the vector packets themselves decode to the note message
        val r2 = Reassembler()
        var got: ByteArray? = null
        for (h in expected) got = r2.feed(hex(h)) ?: got
        val m = MessageCodec.decodeBytes(got!!) as Message.Note
        assertEquals("Standup", m.title)
        assertEquals(2, m.actions.size)
    }

    @Test fun singlePacketUsesFlag03() {
        val p = Framing.split("{\"t\":\"x\"}", 247)
        assertEquals(1, p.size)
        assertEquals(0x03, p[0][0].toInt())
    }

    @Test fun firstPacketDropsHalfBuiltMessage() {
        val r = Reassembler()
        assertNull(r.feed(byteArrayOf(0x01, 'a'.code.toByte(), 'b'.code.toByte())))
        val out = r.feed(byteArrayOf(0x03, 'z'.code.toByte()))
        assertEquals("z", String(out!!))
        // continuation without FIRST is ignored
        assertNull(r.feed(byteArrayOf(0x02, 'q'.code.toByte())))
        assertEquals(1, r.dropped)
    }

    @Test fun oversizeMessageRejected() {
        assertThrows(IllegalArgumentException::class.java) { Framing.split(ByteArray(4097), 247) }
        val r = Reassembler()
        var res: ByteArray? = null
        r.feed(byteArrayOf(0x01) + ByteArray(200))
        repeat(25) { res = r.feed(byteArrayOf(0x00) + ByteArray(200)) }
        assertNull(res)
        assertNull(r.feed(byteArrayOf(0x02, 1)))
        assertTrue(r.dropped >= 1)
        assertEquals(4096, Framing.split(ByteArray(4096), 247).sumOf { it.size - 1 })
    }

    @Test fun adpcmBitExact() {
        val a = vectors["adpcm"]!!.jsonObject
        val blocks = a["blocks_hex"]!!.jsonArray.map { hex(it.jsonPrimitive.content) }
        val expected = a["decoded"]!!.jsonArray.map { it.jsonPrimitive.int }
        val got = blocks.flatMap { ImaAdpcm.decodeBlock(it)!!.map { s -> s.toInt() } }
        assertEquals(512, got.size)
        assertEquals(expected, got)
        // the stream decoder gives the same samples with no gaps
        val st = AdpcmStream()
        blocks.forEach { st.feed(it) }
        assertEquals(expected, st.samples().map { it.toInt() })
        assertEquals(0, st.lostBlocks)
    }

    @Test fun badBlockSizeIgnored() {
        assertNull(ImaAdpcm.decodeBlock(ByteArray(10)))
        assertEquals(0, AdpcmStream().feed(ByteArray(10)).size)
    }

    @Test fun seqGapInsertsZeros() {
        val blocks = vectors["adpcm"]!!.jsonObject["blocks_hex"]!!.jsonArray.map { hex(it.jsonPrimitive.content) }
        // block 0 has seq 0; make a copy with seq 3 (blocks 1 and 2 lost)
        val b3 = blocks[1].copyOf().also { it[0] = 3 }
        val st = AdpcmStream()
        st.feed(blocks[0])
        val added = st.feed(b3)
        assertEquals(2 * 256 + 256, added.size)
        assertTrue(added.take(512).all { it.toInt() == 0 })
        assertEquals(2, st.lostBlocks)
        assertEquals(256 * 4, st.sampleCount)
    }

    @Test fun seqWrapIsNotAGap() {
        val b = vectors["adpcm"]!!.jsonObject["blocks_hex"]!!.jsonArray.map { hex(it.jsonPrimitive.content) }[0]
        val st = AdpcmStream()
        st.feed(b.copyOf().also { it[0] = 255.toByte() })
        st.feed(b.copyOf().also { it[0] = 0 })
        assertEquals(0, st.lostBlocks)
        assertEquals(512, st.sampleCount)
    }

    @Test fun everyMessageRoundTrips() {
        val all: List<Message> = listOf(
            Message.Time(1763000000, -18000),
            Message.Notif(7, "WhatsApp", "Mum", "Call me \"later\" é", listOf("OK", "On my way", "Call you later")),
            Message.NotifRm(7), Message.Weather(-3, "Snow", 1, -6),
            Message.StepsReq, Message.HelloReq, Message.RecsReq, Message.WifiReq, Message.WifiOff,
            Message.Note("M_1.wav", "Standup", "Shipped.", listOf("Buy card", "Test battery")),
            Message.TrCfg("en", "fr"), Message.Tr("them", "Bonjour", "Hello"),
            Message.AppLs, Message.AppPut("timer2", 1200), Message.AppData("AAEC"), Message.AppEnd, Message.AppRm("timer2"),
            Message.FindWatch, Message.PracticeReq,
            Message.Hello("1.0", "Watch-ABCD", 87, 4012, true, 1234),
            Message.Steps(listOf(Message.StepDay(20261114, 1234), Message.StepDay(20261113, 9000))),
            Message.Reply(7, 2), Message.Dismiss(7),
            Message.Recs(listOf(Message.RecFile("M_20261114_093000.wav", 123456, listOf(1000, 5000)))),
            Message.Wifi("Watch-ABCD", "pw123", "192.168.4.1", null, "0123456789abcdef"), Message.Wifi("", "", "", "no sd"),
            Message.RecDel("M_1.wav", 123456), Message.Err("denied on watch", "app_end"),
            Message.Ptt("me", true, 16000, "ima-adpcm"), Message.Ptt("them", false, 16000, "ima-adpcm", 123),
            Message.Practice(listOf(Message.PracticeSession(20261114, "18:30", 25, "metronome", 120, 0))),
            Message.FindPhone(true), Message.MusicCmd("vol_up"),
            Message.Apps(listOf("a", "b")), Message.AppOk("a"), Message.Err("boom"),
        )
        for (m in all) {
            val text = MessageCodec.encode(m)
            val back = MessageCodec.decode(text)
            assertEquals("round trip of $text", m, back)
            // and through framing at a small MTU
            val r = Reassembler()
            var out: ByteArray? = null
            for (p in Framing.split(text, 23)) out = r.feed(p) ?: out
            assertEquals(m, MessageCodec.decodeBytes(out!!))
        }
    }

    @Test fun wireFormatMatchesSpec() {
        assertEquals("{\"t\":\"steps_req\"}", MessageCodec.encode(Message.StepsReq))
        val o = Json.parseToJsonElement(MessageCodec.encode(Message.Time(5, -18000))).jsonObject
        assertEquals(5, o["epoch"]!!.jsonPrimitive.long)
        assertEquals(-18000, o["tz"]!!.jsonPrimitive.int)
        val p = Json.parseToJsonElement(MessageCodec.encode(Message.Practice(listOf(Message.PracticeSession(1, "x", 2, "t", 3, 4))))).jsonObject
        assertEquals(3, p["sessions"]!!.jsonArray[0].jsonObject["bpm_max"]!!.jsonPrimitive.int)
    }

    @Test fun securityFieldsOnTheWire() {
        val w = MessageCodec.decode("{\"t\":\"wifi\",\"ssid\":\"Watch-1\",\"pass\":\"abcdefghijkl\",\"ip\":\"192.168.4.1\",\"token\":\"0011223344556677\"}") as Message.Wifi
        assertEquals("0011223344556677", w.token)
        assertEquals(12, w.pass.length)
        val e = MessageCodec.decode("{\"t\":\"err\",\"re\":\"wifi_req\",\"msg\":\"denied on watch\"}") as Message.Err
        assertEquals("wifi_req", e.re)
        assertEquals(87, (MessageCodec.decode("{\"t\":\"ptt\",\"side\":\"me\",\"on\":false,\"blocks\":87}") as Message.Ptt).blocks)
        assertEquals(-1, (MessageCodec.decode("{\"t\":\"ptt\",\"side\":\"me\",\"on\":true}") as Message.Ptt).blocks) // old firmware
        assertEquals("{\"t\":\"rec_del\",\"n\":\"M_1.wav\",\"s\":42}", MessageCodec.encode(Message.RecDel("M_1.wav", 42)))
    }

    @Test fun unknownTypesAndFieldsAreIgnored() {
        assertEquals(Message.Unknown("future"), MessageCodec.decode("{\"t\":\"future\",\"x\":1}"))
        assertEquals(Message.Dismiss(4), MessageCodec.decode("{\"t\":\"dismiss\",\"id\":4,\"extra\":[1,2]}"))
        assertNull(MessageCodec.decode("not json"))
        assertNull(MessageCodec.decode("{\"no_t\":1}"))
        assertEquals("busy", (MessageCodec.decode("{\"t\":\"wifi\",\"err\":\"busy\"}") as Message.Wifi).err)
    }

    @Test fun appUploadChunks() {
        val src = ByteArray(3500) { (it % 200).toByte() }
        val msgs = AppUpload.messages("my-app", src)
        assertEquals(Message.AppPut("my-app", 3500), msgs.first())
        assertEquals(Message.AppEnd, msgs.last())
        val data = msgs.filterIsInstance<Message.AppData>()
        assertEquals(3, data.size)
        val decoded = data.map { java.util.Base64.getDecoder().decode(it.b64) }
        assertTrue(decoded.all { it.size <= 1536 })
        assertArrayEquals(src, decoded.fold(ByteArray(0)) { a, b -> a + b })
        assertEquals("my_timer", AppUpload.cleanName("My Timer.lua"))
        assertFalse(AppUpload.validName("Bad Name"))
        msgs.forEach { Framing.split(MessageCodec.encodeBytes(it), 247) } // each fits the 4096 limit
    }

    @Test fun stepsMapping() {
        val zone = ZoneId.of("America/Toronto")
        val now = Instant.parse("2026-11-14T20:00:00Z")
        val recs = StepsMapping.toRecords(
            listOf(Message.StepDay(20261114, 1234), Message.StepDay(20261113, 9000), Message.StepDay(20261112, 0), Message.StepDay(20269999, 5)),
            zone, now,
        )
        assertEquals(2, recs.size)
        val today = recs[0]
        assertEquals("ember-steps-20261114", today.clientRecordId)
        assertEquals(1234L, today.count)
        assertEquals(Instant.parse("2026-11-14T05:00:00Z"), today.start)
        assertEquals(now, today.end) // today ends "now", never in the future
        val y = recs[1]
        assertEquals(Instant.parse("2026-11-14T05:00:00Z"), y.end)
        assertEquals(-18000, y.zoneOffsetSeconds)
        val again = StepsMapping.toRecords(listOf(Message.StepDay(20261114, 1300), Message.StepDay(20261113, 9000)), zone, now)
        assertEquals(recs.map { it.clientRecordId }, again.map { it.clientRecordId }) // same ids -> update, not duplicate
    }

    @Test fun luaCatalogFilesAreValid() {
        val dir = File("src/main/assets/lua")
        assertTrue("lua dir ${dir.absolutePath}", dir.isDirectory)
        val files = dir.listFiles { f -> f.extension == "lua" }!!.map { it.nameWithoutExtension }.sorted()
        assertEquals(LuaCatalog.entries.map { it.name }.sorted(), files)
        assertTrue(files.size >= 4)
        for (e in LuaCatalog.entries) {
            val src = File(dir, "${e.name}.lua").readText()
            assertTrue("${e.name} has start()", Regex("(?m)^function start\\(\\)").containsMatchIn(src))
            assertTrue("${e.name} valid name", AppUpload.validName(e.name))
            assertTrue("${e.name} icon header", Regex("(?m)^-- icon: \\w+").containsMatchIn(src))
            assertTrue("${e.name} small enough", src.length < 6000)
            assertFalse("${e.name} no os/io", Regex("\\b(os|io)\\.").containsMatchIn(src))
            val code = src.lines().filterNot { it.trim().startsWith("--") }.joinToString("\n").replace(Regex("'[^'\\n]*'"), "''")
            val opens = Regex("\\b(function|if|for|while)\\b").findAll(code).count()
            val ends = Regex("\\bend\\b").findAll(code).count()
            assertEquals("${e.name} block balance", opens, ends)
            Regex("w\\.every\\((\\d+)").findAll(src).forEach { assertTrue(it.groupValues[1].toInt() >= 100) }
        }
    }
}
