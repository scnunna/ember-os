package com.nunna.embercompanion

import com.nunna.embercompanion.link.*
import com.nunna.embercompanion.protocol.*
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test

class FakeAmberApi : AmberApi {
    var online = true
    val sent = ArrayList<String>()
    val acks = ArrayList<String>()
    var inboxBody = """{"items":[]}"""
    var inboxCalls = 0
    override suspend fun send(bodyJson: String): LifeOsResult {
        if (!online) return LifeOsResult(false, "OFFLINE")
        sent += bodyJson
        return LifeOsResult(true, "OK", 200, """{"ok":true,"id":"11111111-2222-3333-4444-555555555555","ack":"Got it."}""")
    }
    override suspend fun inbox(): LifeOsResult { inboxCalls++; return if (online) LifeOsResult(true, "OK", 200, inboxBody) else LifeOsResult(false, "OFFLINE") }
    override suspend fun inboxAck(bodyJson: String): LifeOsResult { acks += bodyJson; return LifeOsResult(true, "OK", 200) }
}

class AmberFlowTest {
    private var now = 1_794_744_000_000L
    private var connected = true
    private val api = FakeAmberApi()
    private val toWatch = ArrayList<Message>()
    private val flow = AmberFlow(api, { toWatch += it }, { connected }, { now }, { -18000 })

    @Test fun voiceHeardThenPostedThenGotIt() = runBlocking {
        flow.heardVoice("move gym to 6 pm", Message.PttCtx(17, 720, 20261115))
        assertEquals(Message.AmberHeard("move gym to 6 pm"), toWatch[0])
        assertEquals(Message.Amber("11111111-2222-3333-4444-555555555555", "Got it.", false), toWatch[1])
        val body = api.sent.single()
        assertTrue(body, body.contains("\"text\":\"move gym to 6 pm\""))
        assertTrue(body, body.contains("\"at\":1794744000"))
        assertTrue(body, body.contains("\"ctx\":{\"ci_seq\":17,\"slot\":720,\"date\":\"2026-11-15\"}"))
        assertEquals(listOf(false), flow.lines.value.map { it.fromAmber })
    }

    @Test fun noCtxMeansNoCtxField() = runBlocking {
        flow.submit("add call the bank")
        assertFalse(api.sent.single().contains("ctx"))
    }

    @Test fun emptyTranscriptIsReportedNotSent() = runBlocking {
        flow.heardVoice("  ", null)
        assertTrue(api.sent.isEmpty())
        assertTrue((toWatch.single() as Message.Amber).final)
    }

    @Test fun offlineTellsTheWatch() = runBlocking {
        api.online = false
        flow.heardVoice("hello", null)
        val last = toWatch.last() as Message.Amber
        assertTrue(last.final); assertEquals(AmberFlow.NO_REPLY, last.text)
        assertTrue(flow.lines.value.last().error)
    }

    @Test fun pollsEvery20sFor3MinutesThenEvery5Minutes() = runBlocking {
        flow.submit("x")
        api.inboxCalls = 0
        now += 19_000; flow.tick(); assertEquals(0, api.inboxCalls)
        now += 1_000; flow.tick(); assertEquals(1, api.inboxCalls)
        now += 20_000; flow.tick(); assertEquals(2, api.inboxCalls)
        now += 20_000; flow.tick(); assertEquals(3, api.inboxCalls)
        now += 4 * 60_000; flow.tick(); assertEquals(4, api.inboxCalls) // window over: slow mode
        now += 60_000; flow.tick(); assertEquals(4, api.inboxCalls)
        now += 4 * 60_000; flow.tick(); assertEquals(5, api.inboxCalls)
    }

    @Test fun noPollingWhileDisconnected() = runBlocking {
        connected = false
        now += 10 * 60_000; flow.tick()
        assertEquals(0, api.inboxCalls)
    }

    @Test fun repliesGoToTheWatchThenAreAcked() = runBlocking {
        api.inboxBody = """{"items":[{"id":"a1","text":"Moved Gym to 18:00.","reply_to":"r","created_at":"x"},{"id":"a2","text":"${"y".repeat(400)}"}]}"""
        now += 6 * 60_000
        assertEquals(2, flow.tick())
        val m = toWatch.filterIsInstance<Message.Amber>()
        assertEquals(Message.Amber("a1", "Moved Gym to 18:00.", true), m[0])
        assertEquals(300, m[1].text.length)
        assertTrue(m.all { it.final })
        assertEquals("""{"ids":["a1","a2"]}""", api.acks.single())
        // a repeat (the ack failed earlier) is not shown twice
        toWatch.clear(); now += 6 * 60_000
        assertEquals(2, flow.tick())
        assertTrue(toWatch.isEmpty())
        assertEquals(2, flow.lines.value.count { it.fromAmber })
    }

    @Test fun offlineInboxLeavesEverythingAlone() = runBlocking {
        api.online = false; now += 6 * 60_000
        assertEquals(0, flow.tick())
        assertTrue(api.acks.isEmpty())
    }
}
