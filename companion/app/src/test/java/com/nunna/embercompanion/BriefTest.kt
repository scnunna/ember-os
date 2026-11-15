package com.nunna.embercompanion

import com.nunna.embercompanion.link.LifeMath
import com.nunna.embercompanion.protocol.Message
import com.nunna.embercompanion.protocol.MessageCodec
import org.junit.Assert.*
import org.junit.Test

/** Amber's morning summary: /api/watch/today "brief" -> sched.brief -> the watch's 08:00 check-in. */
class BriefTest {
    private val blocks = """[{"id":"a","s":540,"e":600,"title":"WFF","done":false}]"""

    @Test fun briefChangesGenSoThePlanIsResent() {
        val without = LifeMath.parsePlan("""{"date":"2026-11-15","gen":"abcd1234","blocks":$blocks}""")!!
        val with = LifeMath.parsePlan("""{"date":"2026-11-15","gen":"abcd1234","blocks":$blocks,"brief":"Today: 1 block. First: WFF 09:00."}""")!!
        assertEquals("abcd1234", without.gen)
        assertEquals("", without.brief)
        assertEquals("Today: 1 block. First: WFF 09:00.", with.brief)
        assertNotEquals(without.gen, with.gen)
        assertEquals(8, with.gen.length)
        val again = LifeMath.parsePlan("""{"date":"2026-11-15","gen":"abcd1234","blocks":$blocks,"brief":"Today: 1 block. First: WFF 09:00."}""")!!
        assertEquals("same brief = same gen (no needless re-send)", with.gen, again.gen)
    }

    @Test fun briefIsCleanedAndCut() {
        val long = "x".repeat(500)
        val p = LifeMath.parsePlan("""{"date":"2026-11-15","gen":"g","blocks":[],"brief":"a\nb\t$long"}""")!!
        assertEquals(200, p.brief.codePointCount(0, p.brief.length))
        assertFalse(p.brief.any { it.code < 32 })
        assertEquals("", LifeMath.parsePlan("""{"date":"2026-11-15","gen":"g","blocks":[],"brief":null}""")!!.brief)
    }

    @Test fun schedCarriesBriefOnlyWhenPresent() {
        val m = Message.Sched(20261115, "g", listOf(480), emptyList(), "Today: nothing planned.")
        val json = MessageCodec.encode(m)
        assertTrue(json, json.contains("\"brief\":\"Today: nothing planned.\""))
        assertEquals(m, MessageCodec.decode(json))
        assertFalse(MessageCodec.encode(Message.Sched(20261115, "g", listOf(480), emptyList())).contains("brief"))
    }
}
