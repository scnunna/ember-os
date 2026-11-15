package com.nunna.embercompanion

import com.nunna.embercompanion.link.*
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test

class FakeLifeOs : LifeOsApi {
    var online = true
    var todayBody = ""
    val checkinBatches = ArrayList<String>()
    var todayCalls = 0
    var upto = -1L
    override suspend fun today(dateIso: String, tzMinutes: Int): LifeOsResult {
        todayCalls++
        lastDate = dateIso; lastTz = tzMinutes
        return if (online) LifeOsResult(true, "OK", 200, todayBody) else LifeOsResult(false, "OFFLINE")
    }
    var lastDate = ""; var lastTz = 0
    override suspend fun checkins(bodyJson: String): LifeOsResult {
        if (!online) return LifeOsResult(false, "OFFLINE")
        checkinBatches += bodyJson
        val seqs = Regex("\"seq\":(\\d+)").findAll(bodyJson).map { it.groupValues[1].toLong() }.toList()
        return LifeOsResult(true, "OK", 200, upto = if (upto >= 0) upto else seqs.max())
    }
    override suspend fun status() = LifeOsStatus(true, true, "laptop.ts.net")
}

fun ci(seq: Long, slot: Int, ans: String, at: Long, date: Int = 20261115, ids: List<String> = listOf("b1")) = Message.Ci(seq, date, slot, at, ans, ids)

class LifeLinkTest {
    // 2026-11-15 12:00 UTC, phone at UTC-5 -> local 07:00 on 2026-11-15
    private var now = 1_794_744_000_000L
    private val tz = -18000
    private val watch = FakeWatch()
    private val api = FakeLifeOs()
    private val persisted = ArrayList<List<Checkin>>()
    private val order = ArrayList<String>()
    private val log = CheckinLog(emptyList(), { persisted += it; order += "persist" })
    private val link = LinkController(watch, { now }, { tz }, { null }, lifeOs = api, checkinLog = log, ciConfig = { Message.CiCfg(true, 15) })
        .also { watch.link = it }

    private fun body(gen: String, date: String = "2026-11-15") =
        """{"date":"$date","gen":"$gen","slots":[480,720,1440],"blocks":[{"id":"b2","s":600,"e":660,"title":"Gym","done":false},{"id":"b1","s":420,"e":600,"title":"WFF deep work","done":false}]}"""

    private fun connect(sd: Int = 0) {
        link.onReady(247)
        watch.say(Message.Hello("1.4", "Watch-AB", 80, 4000, false, 1, sd, 0))
    }
    private fun scheds() = watch.received.filterIsInstance<Message.Sched>()
    private fun tick() = runBlocking { link.lifeTick() }

    @Test fun ciCfgSentOnConnect() {
        connect()
        assertTrue(watch.received.contains(Message.CiCfg(true, 15)))
    }

    @Test fun schedPushedOnHello() {
        api.todayBody = body("aaaa1111")
        connect(sd = 0)
        tick()
        assertEquals("2026-11-15", api.lastDate)
        assertEquals(-300, api.lastTz)
        val s = scheds().single()
        assertEquals(20261115, s.date); assertEquals("aaaa1111", s.gen)
        assertEquals(listOf("b1", "b2"), s.blocks.map { it.id }) // sorted
        assertEquals(listOf(480, 720, 1440), s.slots)
    }

    @Test fun noPushWhenSameGenAndWatchHasToday() {
        api.todayBody = body("aaaa1111")
        connect(sd = 0); tick()
        watch.say(Message.SchedOk(20261115, "aaaa1111", 2))
        // reconnect: watch reports the stored plan is today's
        link.onDisconnected(); watch.received.clear()
        connect(sd = 20261115); tick()
        assertEquals(2, api.todayCalls)
        assertTrue(scheds().isEmpty())
    }

    @Test fun pushWhenWatchPlanIsStale() {
        api.todayBody = body("aaaa1111")
        connect(sd = 0); tick()
        link.onDisconnected(); watch.received.clear()
        connect(sd = 20261114); tick() // same gen but the watch holds yesterday
        assertEquals(1, scheds().size)
    }

    @Test fun rePushOnGenChangeAfterFifteenMinutes() {
        api.todayBody = body("aaaa1111")
        connect(sd = 0); tick()
        watch.say(Message.SchedOk(20261115, "aaaa1111", 2))
        now += 14 * 60_000; tick()
        assertEquals(1, api.todayCalls) // not yet
        api.todayBody = body("bbbb2222")
        now += 2 * 60_000; tick()
        assertEquals(2, api.todayCalls)
        assertEquals(listOf("aaaa1111", "bbbb2222"), scheds().map { it.gen })
        now += 15 * 60_000; tick() // same gen again: no third push
        assertEquals(2, scheds().size)
    }

    @Test fun fetchesAfterLocalMidnight() {
        api.todayBody = body("aaaa1111")
        connect(sd = 0); tick()
        watch.say(Message.SchedOk(20261115, "aaaa1111", 2))
        now += 17 * 3_600_000L + 1 * 60_000 // 00:01 local next day (still within 15 min of nothing: forced by date change)
        api.todayBody = body("cccc3333", "2026-11-16")
        tick()
        assertEquals("2026-11-16", api.lastDate)
        assertEquals(20261116, scheds().last().date)
    }

    @Test fun offlineFetchDoesNotPushAndRetries() {
        api.online = false
        connect(); tick()
        assertTrue(scheds().isEmpty())
        assertEquals("OFFLINE", link.lifeError.value)
        api.online = true; api.todayBody = body("aaaa1111")
        now += 61_000; tick()
        assertEquals(1, scheds().size)
        assertNull(link.lifeError.value)
    }

    @Test fun ciIsStoredBeforeTheAck() {
        connect()
        watch.received.clear()
        val log2 = CheckinLog(emptyList(), { order += "persist" })
        val w = FakeWatch(); val order2 = order
        val l = LinkController(object : LinkTransport { override fun write(packets: List<ByteArray>) { order2 += "write"; w.write(packets) } },
            { now }, { tz }, { null }, lifeOs = api, checkinLog = log2).also { w.link = it }
        l.onReady(247); order.clear()
        w.say(ci(17, 720, "on", 1_794_744_000))
        assertEquals(listOf("persist", "write"), order)
        assertEquals(Message.CiAck(17), w.received.last())
        assertEquals(1, log2.all.value.size)
    }

    @Test fun duplicateCiIsAckedButStoredOnce() {
        connect()
        val c = ci(5, 720, "on", 1_794_744_000)
        watch.say(c); watch.say(c)
        assertEquals(1, log.all.value.size)
        assertEquals(2, watch.received.count { it is Message.CiAck })
    }

    @Test fun queueDrainsInBatchesOf64AndSurvivesOffline() {
        connect()
        for (i in 1..130) watch.say(ci(i.toLong(), 480, "on", 1_794_744_000L + i))
        assertEquals(130, log.pendingCount())
        api.online = false
        tick()
        assertEquals(130, log.pendingCount())
        assertEquals("OFFLINE", link.lifeError.value)
        api.online = true
        tick() // still inside the backoff window: nothing happens
        assertEquals(130, log.pendingCount())
        now += 31_000; tick()
        assertEquals(0, log.pendingCount())
        assertEquals(listOf(64, 64, 2), api.checkinBatches.map { Regex("\"seq\"").findAll(it).count() })
        assertTrue(api.checkinBatches[0].startsWith("{\"items\":[{\"seq\":1,\"date\":20261115,\"slot\":480,\"at\":"))
        assertTrue(api.checkinBatches[0].contains("\"ans\":\"on\",\"ids\":[\"b1\"]"))
    }

    @Test fun reconnectResetsBackoffAndDrains() {
        connect()
        watch.say(ci(1, 720, "off", 1_794_744_000))
        api.online = false; tick()
        api.online = true
        link.onDisconnected(); connect()
        tick()
        assertEquals(0, log.pendingCount())
    }

    @Test fun drainWorksEvenWhenWatchIsDisconnected() {
        connect()
        watch.say(ci(1, 720, "on", 1_794_744_000))
        api.online = false; tick()
        link.onDisconnected()
        api.online = true; now += 31_000
        tick()
        assertEquals(0, log.pendingCount())
    }

    @Test fun keepsAtMost500AndPrefersDroppingSent() {
        val l = CheckinLog(emptyList(), {})
        for (i in 1..500) l.append(Checkin(i.toLong(), 20261115, 480, 1000L + i, "on"))
        l.markSent(setOf(Checkin(1, 20261115, 480, 1001, "on").key))
        l.append(Checkin(501, 20261115, 480, 2000, "on"))
        assertEquals(500, l.all.value.size)
        assertFalse(l.all.value.any { it.seq == 1L }) // the sent one went first
        l.append(Checkin(502, 20261115, 480, 2001, "on"))
        assertEquals(500, l.all.value.size)
        assertFalse(l.all.value.any { it.seq == 2L })
    }

    @Test fun adherenceAndNowLine() {
        connect(); api.todayBody = body("aaaa1111"); tick()
        val t = now / 1000
        watch.say(ci(1, 480, "on", t - 3600))
        watch.say(ci(2, 720, "snooze", t - 1800))
        watch.say(ci(3, 960, "off", t - 1700))
        watch.say(ci(4, 1200, "missed", t - 1600))
        watch.say(ci(5, 1440, "off", t - 86400 * 2, date = 20261113)) // another day
        val a = link.adherence()!!
        assertEquals(Adherence(1, 1, 1, 1), a)
        assertEquals(33, a.percent)
        // 07:00 local: block 07:00-10:00 is current, 180 min left
        assertEquals("Now: WFF deep work, 180 min left", link.nowLine())
        now += 3 * 3_600_000L + 60_000 // 10:01
        assertEquals("Now: Gym, 59 min left", link.nowLine())
        now += 2 * 3_600_000L
        assertNull(link.nowLine())
    }

    @Test fun nextLineWhenBetweenBlocks() {
        val p = Plan(20261115, "g", listOf(480), listOf(Message.SchedBlock("a", 840, 900, "Call", false)))
        assertEquals("Next: Call 14:00", LifeMath.nowLine(p, now, tz))
        assertNull(LifeMath.nowLine(p.copy(date = 20261114), now, tz))
    }

    @Test fun planParsingCutsTitlesAndLimitsBlocks() {
        val long = "é".repeat(50)
        val blocks = (0 until 30).joinToString(",") { """{"id":"i$it","s":${it * 10},"e":${it * 10 + 5},"title":"$long","done":${it == 0}}""" }
        val p = LifeMath.parsePlan("""{"date":"2026-11-15","gen":"0123456789abcdef","blocks":[$blocks]}""")!!
        assertEquals(24, p.blocks.size)
        assertEquals(39, p.blocks[0].title.length)
        assertEquals("01234567", p.gen)
        assertEquals(LifeMath.DEFAULT_SLOTS, p.slots) // no slots in the reply -> the defaults
        assertEquals(listOf(480, 1440), LifeMath.parsePlan("""{"date":"2026-11-15","gen":"x","slots":[1440,480,480,9999],"blocks":[]}""")!!.slots)
        assertTrue(p.blocks[0].done)
        assertNull(LifeMath.parsePlan("not json"))
        assertNull(LifeMath.parsePlan("""{"date":"nope","gen":"x","blocks":[]}"""))
    }

    @Test fun framingAtMtu23ForLifeMessages() {
        val w = FakeWatch(23)
        val l = LinkController(w, { now }, { tz }, { null }, lifeOs = api, checkinLog = CheckinLog(emptyList(), {})).also { w.link = it }
        l.onReady(23)
        w.say(Message.Hello("1.4", "W", 1, 2, false, 3, 0, 0))
        w.say(ci(9, 720, "on", 1_794_744_000, ids = listOf("0b9e4c7a-1111-4222-8333-444455556666")))
        assertEquals(Message.CiAck(9), w.received.last())
        api.todayBody = body("aaaa1111")
        runBlocking { l.lifeTick() }
        assertEquals(1, w.received.filterIsInstance<Message.Sched>().size)
    }
}
