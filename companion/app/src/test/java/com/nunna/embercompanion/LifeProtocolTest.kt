package com.nunna.embercompanion

import com.nunna.embercompanion.protocol.*
import org.junit.Assert.*
import org.junit.Test

class LifeProtocolTest {
    private val id = "0b9e4c7a-1111-4222-8333-444455556666"

    @Test fun roundTripsAlsoThroughFramingAtMtu23() {
        val all: List<Message> = listOf(
            Message.Hello("1.4", "Watch-ABCD", 87, 4012, false, 10, 20261115, 3),
            Message.Sched(20261115, "9f2c1a7b", listOf(480, 720, 960, 1200, 1440), listOf(Message.SchedBlock(id, 540, 600, "WFF deep work é", false), Message.SchedBlock("x", 600, 1440, "Late", true))),
            Message.Sched(20261115, "", listOf(480), emptyList()),
            Message.CiAck(4_000_000_000L), Message.CiCfg(true, 15), Message.CiCfg(false, 60),
            Message.Ci(4_000_000_001L, 20261115, 720, 1763215200, "on", listOf(id, "b2")),
            Message.Ci(5, 20261114, 1440, 1763215200, "missed", emptyList()),
            Message.SchedOk(20261115, "9f2c1a7b", 2),
            Message.AmberHeard("move gym to 6 pm \"please\" é"),
            Message.Amber(id, "Got it.", false), Message.Amber("local", "Done: moved Gym to 18:00.", true),
            Message.Ptt("amber", true, 16000, "ima-adpcm"),
            Message.Ptt("amber", true, 16000, "ima-adpcm", -1, Message.PttCtx(17, 720, 20261115)),
            Message.Ptt("amber", false, 16000, "ima-adpcm", 40),
        )
        for (m in all) {
            val text = MessageCodec.encode(m)
            assertEquals(text, m, MessageCodec.decode(text))
            val r = Reassembler(); var out: ByteArray? = null
            for (p in Framing.split(text, 23)) out = r.feed(p) ?: out
            assertEquals(m, MessageCodec.decodeBytes(out!!))
        }
    }

    @Test fun fieldNamesMatchTheContract() {
        assertEquals(
            "{\"t\":\"sched\",\"date\":20261115,\"gen\":\"9f2c1a7b\",\"slots\":[480,1440],\"blocks\":[{\"id\":\"id1\",\"s\":540,\"e\":600,\"title\":\"T\",\"done\":false}]}",
            MessageCodec.encode(Message.Sched(20261115, "9f2c1a7b", listOf(480, 1440), listOf(Message.SchedBlock("id1", 540, 600, "T", false)))),
        )
        assertEquals("{\"t\":\"ci_ack\",\"upto\":17}", MessageCodec.encode(Message.CiAck(17)))
        assertEquals("{\"t\":\"ci_cfg\",\"on\":true,\"snooze\":15}", MessageCodec.encode(Message.CiCfg(true, 15)))
        assertEquals("{\"t\":\"amber_heard\",\"text\":\"hi\"}", MessageCodec.encode(Message.AmberHeard("hi")))
        assertEquals("{\"t\":\"amber\",\"id\":\"a1\",\"text\":\"Got it.\",\"final\":false}", MessageCodec.encode(Message.Amber("a1", "Got it.", false)))
        assertEquals(Message.Hello("1.3", "W", 1, 2, false, 3, 0, 0), MessageCodec.decode("{\"t\":\"hello\",\"fw\":\"1.3\",\"name\":\"W\",\"batt\":1,\"mv\":2,\"usb\":false,\"steps\":3}"))
        assertEquals(
            Message.Ci(17, 20261115, 720, 1763215200, "snooze", listOf("a", "b")),
            MessageCodec.decode("{\"t\":\"ci\",\"seq\":17,\"date\":20261115,\"slot\":720,\"at\":1763215200,\"ans\":\"snooze\",\"ids\":[\"a\",\"b\"]}"),
        )
        assertEquals(Message.SchedOk(20261115, "g", 2), MessageCodec.decode("{\"t\":\"sched_ok\",\"date\":20261115,\"gen\":\"g\",\"n\":2}"))
        assertEquals(
            Message.Ptt("amber", true, 16000, "ima-adpcm", -1, Message.PttCtx(17, 720, 20261115)),
            MessageCodec.decode("{\"t\":\"ptt\",\"side\":\"amber\",\"on\":true,\"ctx\":{\"ci_seq\":17,\"slot\":720,\"date\":20261115}}"),
        )
    }

    @Test fun fullDayPlanFitsOneMessage() {
        val blocks = (0 until 24).map { Message.SchedBlock("0b9e4c7a-1111-4222-8333-44445555%04d".format(it), it * 60, it * 60 + 59, "x".repeat(39), false) }
        val m = Message.Sched(20261115, "9f2c1a7b", listOf(480, 720, 960, 1200, 1440), blocks)
        val r = Reassembler(); var out: ByteArray? = null
        for (p in Framing.split(MessageCodec.encodeBytes(m), 23)) out = r.feed(p) ?: out
        assertEquals(m, MessageCodec.decodeBytes(out!!))
    }
}
