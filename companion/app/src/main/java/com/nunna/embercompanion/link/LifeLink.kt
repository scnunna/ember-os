package com.nunna.embercompanion.link

import com.nunna.embercompanion.protocol.Message
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.json.*
import java.time.LocalDate

/** Result of one Life OS call through Ember Gateway. [code] is one of the Contract codes (OK, NOT_CONFIGURED, HTTP_ERROR, OFFLINE, ...). */
data class LifeOsResult(val ok: Boolean, val code: String, val status: Int = 0, val body: String = "", val upto: Long = -1, val message: String = "")

/** Whether the Gateway app is installed and its Life OS link is set up (never includes the token). */
data class LifeOsStatus(val gatewayInstalled: Boolean, val configured: Boolean, val host: String = "")

/** What the companion may ask of Life OS. The real one goes through Ember Gateway; tests use a fake. */
interface LifeOsApi {
    suspend fun today(dateIso: String, tzMinutes: Int): LifeOsResult
    suspend fun checkins(bodyJson: String): LifeOsResult
    suspend fun status(): LifeOsStatus
}

/** One answer from the watch. [sent] = the Gateway took it (kept for the adherence figure). */
data class Checkin(val seq: Long, val date: Int, val slot: Int, val at: Long, val ans: String, val ids: List<String> = emptyList(), val sent: Boolean = false) {
    val key get() = "$seq|$date|$slot|$at"
}

/**
 * The local check-in log + upload queue (pure Kotlin). Every [append] calls [persist] before returning, so the caller
 * can acknowledge the watch right after. Keeps at most [max] rows; when full, already-sent rows go first.
 */
class CheckinLog(initial: List<Checkin>, private val persist: (List<Checkin>) -> Unit, private val max: Int = 500) {
    private val _all = MutableStateFlow(initial.takeLast(max))
    val all: StateFlow<List<Checkin>> = _all.asStateFlow()

    /** True when stored (false when it was already there). The data is persisted before this returns. */
    @Synchronized fun append(c: Checkin): Boolean {
        if (_all.value.any { it.key == c.key }) return false
        val l = (_all.value + c).toMutableList()
        while (l.size > max) { val i = l.indexOfFirst { it.sent }; l.removeAt(if (i >= 0) i else 0) }
        persist(l)
        _all.value = l
        return true
    }

    @Synchronized fun unsent(limit: Int): List<Checkin> = _all.value.filter { !it.sent }.take(limit)
    @Synchronized fun pendingCount(): Int = _all.value.count { !it.sent }

    @Synchronized fun markSent(keys: Set<String>) {
        val l = _all.value.map { if (it.key in keys) it.copy(sent = true) else it }
        persist(l)
        _all.value = l
    }
}

/** A day's plan as the phone knows it. */
data class Plan(val date: Int, val gen: String, val slots: List<Int>, val blocks: List<Message.SchedBlock>)

data class Adherence(val on: Int, val off: Int, val missed: Int, val snoozing: Int) {
    val total get() = on + off + missed
    /** null when nothing has been answered yet. */
    val percent: Int? get() = if (total == 0) null else Math.round(on * 100f / total)
}

object LifeMath {
    val DEFAULT_SLOTS = listOf(480, 720, 960, 1200, 1440)

    fun localSeconds(nowMs: Long, tzSec: Int) = Math.floorDiv(nowMs, 1000L) + tzSec
    fun localDate(nowMs: Long, tzSec: Int): LocalDate = LocalDate.ofEpochDay(Math.floorDiv(localSeconds(nowMs, tzSec), 86_400L))
    fun dateInt(d: LocalDate) = d.year * 10000 + d.monthValue * 100 + d.dayOfMonth
    fun localDateInt(nowMs: Long, tzSec: Int) = dateInt(localDate(nowMs, tzSec))
    fun minuteOfDay(nowMs: Long, tzSec: Int) = (Math.floorMod(localSeconds(nowMs, tzSec), 86_400L) / 60).toInt()

    /** "2026-11-15" -> 20261115, or null if it is not a valid date. */
    fun isoToInt(s: String): Int? = runCatching { dateInt(LocalDate.parse(s)) }.getOrNull()

    /** Today's adherence from the local log: the latest answer per slot of today counts; snooze is not final. */
    fun adherence(log: List<Checkin>, nowMs: Long, tzSec: Int): Adherence {
        val today = localDateInt(nowMs, tzSec)
        val latest = log.filter { it.date == today }
            .groupBy { it.slot }.map { (_, l) -> l.maxWith(compareBy<Checkin>({ it.at }, { it.seq })) }
        return Adherence(latest.count { it.ans == "on" }, latest.count { it.ans == "off" }, latest.count { it.ans == "missed" }, latest.count { it.ans == "snooze" })
    }

    /** "Now: WFF deep work, 25 min left" or "Next: Gym 14:00", or null when the plan is not for today / nothing is left. */
    fun nowLine(plan: Plan?, nowMs: Long, tzSec: Int): String? {
        if (plan == null || plan.date != localDateInt(nowMs, tzSec)) return null
        val m = minuteOfDay(nowMs, tzSec)
        val open = plan.blocks.filter { !it.done }.sortedBy { it.s }
        open.firstOrNull { m >= it.s && m < it.e }?.let { return "Now: ${it.title}, ${it.e - m} min left" }
        open.firstOrNull { it.s > m }?.let { return "Next: ${it.title} %02d:%02d".format(it.s / 60, it.s % 60) }
        return null
    }

    /** Parses the server's /api/watch/today body (`date` is YYYY-MM-DD) into a [Plan] for the watch. Null if malformed. */
    fun parsePlan(body: String): Plan? = runCatching {
        val o = Json.parseToJsonElement(body).jsonObject
        val date = isoToInt(o["date"]!!.jsonPrimitive.content) ?: return null
        val gen = o["gen"]!!.jsonPrimitive.content.take(8)
        val blocks = o["blocks"]!!.jsonArray.map {
            val b = it.jsonObject
            Message.SchedBlock(
                b["id"]!!.jsonPrimitive.content.take(36),
                b["s"]!!.jsonPrimitive.int.coerceIn(0, 1440), b["e"]!!.jsonPrimitive.int.coerceIn(0, 1440),
                cut(b["title"]!!.jsonPrimitive.content, 39), b["done"]?.jsonPrimitive?.booleanOrNull ?: false,
            )
        }.sortedBy { it.s }.take(24)
        val slots = (o["slots"] as? JsonArray)?.mapNotNull { it.jsonPrimitive.intOrNull?.takeIf { v -> v in 0..1440 } }?.distinct()?.sorted()?.take(8)
            ?.takeIf { it.isNotEmpty() } ?: DEFAULT_SLOTS
        Plan(date, gen, slots, blocks)
    }.getOrNull()

    /** Cut on characters (code points), never in the middle of one. */
    fun cut(s: String, n: Int): String = if (s.codePointCount(0, s.length) <= n) s else s.substring(0, s.offsetByCodePoints(0, n))

    fun checkinBody(items: List<Checkin>): String = buildJsonObject {
        put("items", JsonArray(items.map {
            buildJsonObject {
                put("seq", it.seq); put("date", it.date); put("slot", it.slot); put("at", it.at); put("ans", it.ans)
                put("ids", JsonArray(it.ids.map { id -> JsonPrimitive(id) }))
            }
        }))
    }.toString()
}
