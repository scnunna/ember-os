package com.nunna.embercompanion.protocol

import kotlinx.serialization.json.*

/** Every Ember Link message. Phone->watch and watch->phone share one hierarchy so each can be round-tripped in tests. */
sealed interface Message {
    // ---- phone -> watch (CMD) ----
    data class Time(val epoch: Long, val tz: Int) : Message
    data class Notif(val id: Int, val app: String, val title: String, val body: String, val replies: List<String>) : Message
    data class NotifRm(val id: Int) : Message
    data class Weather(val temp: Int, val text: String, val hi: Int, val lo: Int) : Message
    data object StepsReq : Message
    data object HelloReq : Message          // v1.2: ask for hello once our notification handler is ready
    data class PhoneInfo(val name: String) : Message   // v1.2: this phone's name for the watch's Devices list
    data object RecsReq : Message
    data object WifiReq : Message
    data object WifiOff : Message
    data class Note(val id: String, val title: String, val summary: String, val actions: List<String>) : Message
    data class TrCfg(val me: String, val them: String) : Message
    data class Tr(val side: String, val src: String, val dst: String) : Message
    data object AppLs : Message
    data class AppPut(val name: String, val size: Int) : Message
    data class AppData(val b64: String) : Message
    data object AppEnd : Message
    data class AppRm(val name: String) : Message
    data object FindWatch : Message
    /** Ask the watch to delete a recording; it only does so if [s] matches the file size. */
    data class RecDel(val n: String, val s: Long) : Message
    data object PracticeReq : Message
    /** v1.4: one block of the day's plan. [s]/[e] are minutes after local midnight. */
    data class SchedBlock(val id: String, val s: Int, val e: Int, val title: String, val done: Boolean)
    /** v1.4: replace the watch's plan for [date] (yyyymmdd). */
    data class Sched(val date: Int, val gen: String, val slots: List<Int>, val blocks: List<SchedBlock>,
        /** v1.4: Amber's morning summary, shown in the 08:00 check-in (max 200 characters, "" = none). */
        val brief: String = "") : Message
    /** v1.4: the watch may drop queued check-ins with seq <= [upto]. */
    data class CiAck(val upto: Long) : Message
    /** v1.4: check-in settings. [snooze] is 5..60 minutes. */
    data class CiCfg(val on: Boolean, val snooze: Int) : Message
    /** v1.4: what the phone understood from a hold-to-talk to Amber (max 300 characters). */
    data class AmberHeard(val text: String) : Message
    /** v1.4: Amber's reply (max 300 characters). [final] = the real answer (buzz); false = just "Got it." */
    data class Amber(val id: String, val text: String, val final: Boolean) : Message

    // ---- watch -> phone (EVT) ----
    data class Hello(val fw: String, val name: String, val batt: Int, val mv: Int, val usb: Boolean, val steps: Int,
        /** v1.4: yyyymmdd of the plan stored on the watch (0 = none) and how many check-ins are queued. */
        val sd: Int = 0, val ciq: Int = 0) : Message
    data class StepDay(val date: Int, val steps: Int)
    data class Steps(val days: List<StepDay>) : Message
    data class Reply(val id: Int, val idx: Int) : Message
    data class Dismiss(val id: Int) : Message
    data class RecFile(val n: String, val s: Long, val marks: List<Long>)
    data class Recs(val files: List<RecFile>) : Message
    /** Either (ssid, pass, ip) or [err]. */
    data class Wifi(val ssid: String, val pass: String, val ip: String, val err: String?, val token: String = "") : Message
    /** [blocks] = number of AUDIO blocks sent (only meaningful on `off`), -1 if the watch did not say. */
    data class PttCtx(val ciSeq: Long, val slot: Int, val date: Int)
    data class Ptt(val side: String, val on: Boolean, val rate: Int, val codec: String, val blocks: Int = -1, val ctx: PttCtx? = null) : Message
    data class PracticeSession(val date: Int, val start: String, val min: Int, val tool: String, val bpmMax: Int, val changes: Int)
    data class Practice(val sessions: List<PracticeSession>) : Message
    data class FindPhone(val on: Boolean) : Message
    data class MusicCmd(val cmd: String) : Message
    data class Apps(val list: List<String>) : Message
    data class AppOk(val name: String) : Message
    /** [re] = the `t` of the message that failed, if the watch says. */
    data class Err(val msg: String, val re: String = "") : Message

    /**
     * v1.4: one answer to a check-in slot. [seq] is a u32 kept by the watch; [date] yyyymmdd the slot belongs to; [slot]
     * minutes after local midnight (1440 = end of day); [at] UTC epoch seconds; [ans] on|off|snooze|missed; [ids] the
     * block ids planned in that window.
     */
    data class Ci(val seq: Long, val date: Int, val slot: Int, val at: Long, val ans: String, val ids: List<String>) : Message
    /** v1.4: reply to [Sched]. */
    data class SchedOk(val date: Int, val gen: String, val n: Int) : Message

    /** A `t` value we do not know. Ignored by receivers, kept for logging. */
    data class Unknown(val t: String) : Message
}

object MessageCodec {
    private val json = Json { ignoreUnknownKeys = true }

    fun encode(m: Message): String = toObject(m).toString()

    fun encodeBytes(m: Message): ByteArray = encode(m).toByteArray(Charsets.UTF_8)

    private fun obj(t: String, block: JsonObjectBuilder.() -> Unit = {}) = buildJsonObject { put("t", t); block() }
    private fun strs(l: List<String>) = JsonArray(l.map { JsonPrimitive(it) })

    fun toObject(m: Message): JsonObject = when (m) {
        is Message.Time -> obj("time") { put("epoch", m.epoch); put("tz", m.tz) }
        is Message.Notif -> obj("notif") {
            put("id", m.id); put("app", m.app); put("title", m.title); put("body", m.body); put("replies", strs(m.replies))
        }
        is Message.NotifRm -> obj("notif_rm") { put("id", m.id) }
        is Message.Weather -> obj("weather") { put("temp", m.temp); put("text", m.text); put("hi", m.hi); put("lo", m.lo) }
        Message.StepsReq -> obj("steps_req")
        Message.HelloReq -> obj("hello_req")
        is Message.PhoneInfo -> obj("phone_info") { put("name", m.name) }
        Message.RecsReq -> obj("recs_req")
        Message.WifiReq -> obj("wifi_req")
        Message.WifiOff -> obj("wifi_off")
        is Message.Note -> obj("note") {
            put("id", m.id); put("title", m.title); put("summary", m.summary); put("actions", strs(m.actions))
        }
        is Message.TrCfg -> obj("tr_cfg") { put("me", m.me); put("them", m.them) }
        is Message.Tr -> obj("tr") { put("side", m.side); put("src", m.src); put("dst", m.dst) }
        Message.AppLs -> obj("app_ls")
        is Message.AppPut -> obj("app_put") { put("name", m.name); put("size", m.size) }
        is Message.AppData -> obj("app_data") { put("b64", m.b64) }
        Message.AppEnd -> obj("app_end")
        is Message.AppRm -> obj("app_rm") { put("name", m.name) }
        Message.FindWatch -> obj("find_watch")
        is Message.RecDel -> obj("rec_del") { put("n", m.n); put("s", m.s) }
        Message.PracticeReq -> obj("practice_req")
        is Message.Sched -> obj("sched") {
            put("date", m.date); put("gen", m.gen)
            put("slots", JsonArray(m.slots.map { JsonPrimitive(it) }))
            put("blocks", JsonArray(m.blocks.map { b ->
                buildJsonObject { put("id", b.id); put("s", b.s); put("e", b.e); put("title", b.title); put("done", b.done) }
            }))
            if (m.brief.isNotEmpty()) put("brief", m.brief)
        }
        is Message.CiAck -> obj("ci_ack") { put("upto", m.upto) }
        is Message.CiCfg -> obj("ci_cfg") { put("on", m.on); put("snooze", m.snooze) }
        is Message.AmberHeard -> obj("amber_heard") { put("text", m.text) }
        is Message.Amber -> obj("amber") { put("id", m.id); put("text", m.text); put("final", m.final) }
        is Message.Ci -> obj("ci") {
            put("seq", m.seq); put("date", m.date); put("slot", m.slot); put("at", m.at); put("ans", m.ans)
            put("ids", strs(m.ids))
        }
        is Message.SchedOk -> obj("sched_ok") { put("date", m.date); put("gen", m.gen); put("n", m.n) }
        is Message.Hello -> obj("hello") {
            put("fw", m.fw); put("name", m.name); put("batt", m.batt); put("mv", m.mv); put("usb", m.usb); put("steps", m.steps)
            put("sd", m.sd); put("ciq", m.ciq)
        }
        is Message.Steps -> obj("steps") {
            put("days", JsonArray(m.days.map { buildJsonObject { put("date", it.date); put("steps", it.steps) } }))
        }
        is Message.Reply -> obj("reply") { put("id", m.id); put("idx", m.idx) }
        is Message.Dismiss -> obj("dismiss") { put("id", m.id) }
        is Message.Recs -> obj("recs") {
            put("files", JsonArray(m.files.map { f ->
                buildJsonObject { put("n", f.n); put("s", f.s); put("marks", JsonArray(f.marks.map { JsonPrimitive(it) })) }
            }))
        }
        is Message.Wifi -> obj("wifi") {
            if (m.err != null) put("err", m.err) else { put("ssid", m.ssid); put("pass", m.pass); put("ip", m.ip); if (m.token.isNotEmpty()) put("token", m.token) }
        }
        is Message.Ptt -> obj("ptt") { put("side", m.side); put("on", m.on); put("rate", m.rate); put("codec", m.codec); if (m.blocks >= 0) put("blocks", m.blocks)
            m.ctx?.let { c -> put("ctx", buildJsonObject { put("ci_seq", c.ciSeq); put("slot", c.slot); put("date", c.date) }) } }
        is Message.Practice -> obj("practice") {
            put("sessions", JsonArray(m.sessions.map { s ->
                buildJsonObject {
                    put("date", s.date); put("start", s.start); put("min", s.min); put("tool", s.tool)
                    put("bpm_max", s.bpmMax); put("changes", s.changes)
                }
            }))
        }
        is Message.FindPhone -> obj("find_phone") { put("on", m.on) }
        is Message.MusicCmd -> obj("music_cmd") { put("cmd", m.cmd) }
        is Message.Apps -> obj("apps") { put("list", strs(m.list)) }
        is Message.AppOk -> obj("app_ok") { put("name", m.name) }
        is Message.Err -> obj("err") { put("msg", m.msg); if (m.re.isNotEmpty()) put("re", m.re) }
        is Message.Unknown -> obj(m.t)
    }

    /** Parses one JSON message. Returns null for malformed JSON or a missing `t`; unknown `t` gives [Message.Unknown]. */
    fun decode(text: String): Message? {
        val o = try { json.parseToJsonElement(text) as? JsonObject } catch (e: Exception) { null } ?: return null
        val t = o.str("t") ?: return null
        return try {
            when (t) {
                "time" -> Message.Time(o.long("epoch"), o.int("tz"))
                "notif" -> Message.Notif(o.int("id"), o.str("app") ?: "", o.str("title") ?: "", o.str("body") ?: "", o.strList("replies"))
                "notif_rm" -> Message.NotifRm(o.int("id"))
                "weather" -> Message.Weather(o.int("temp"), o.str("text") ?: "", o.int("hi"), o.int("lo"))
                "steps_req" -> Message.StepsReq
                "hello_req" -> Message.HelloReq
                "phone_info" -> Message.PhoneInfo(o.str("name") ?: "")
                "recs_req" -> Message.RecsReq
                "wifi_req" -> Message.WifiReq
                "wifi_off" -> Message.WifiOff
                "note" -> Message.Note(o.str("id") ?: "", o.str("title") ?: "", o.str("summary") ?: "", o.strList("actions"))
                "tr_cfg" -> Message.TrCfg(o.str("me") ?: "", o.str("them") ?: "")
                "tr" -> Message.Tr(o.str("side") ?: "", o.str("src") ?: "", o.str("dst") ?: "")
                "app_ls" -> Message.AppLs
                "app_put" -> Message.AppPut(o.str("name") ?: "", o.int("size"))
                "app_data" -> Message.AppData(o.str("b64") ?: "")
                "app_end" -> Message.AppEnd
                "app_rm" -> Message.AppRm(o.str("name") ?: "")
                "find_watch" -> Message.FindWatch
                "rec_del" -> Message.RecDel(o.str("n") ?: "", o.long("s"))
                "practice_req" -> Message.PracticeReq
                "hello" -> Message.Hello(o.str("fw") ?: "", o.str("name") ?: "", o.int("batt"), o.int("mv"), o.bool("usb"), o.int("steps"), o.int("sd"), o.int("ciq"))
                "sched" -> Message.Sched(o.int("date"), o.str("gen") ?: "", (o["slots"] as? JsonArray)?.mapNotNull { it.jsonPrimitive.intOrNull } ?: emptyList(), o.arr("blocks").map {
                    Message.SchedBlock(it.str("id") ?: "", it.int("s"), it.int("e"), it.str("title") ?: "", it.bool("done"))
                }, o.str("brief") ?: "")
                "ci_ack" -> Message.CiAck(o.long("upto"))
                "ci_cfg" -> Message.CiCfg(o.bool("on"), o.int("snooze"))
                "amber_heard" -> Message.AmberHeard(o.str("text") ?: "")
                "amber" -> Message.Amber(o.str("id") ?: "", o.str("text") ?: "", o.bool("final"))
                "ci" -> Message.Ci(o.long("seq"), o.int("date"), o.int("slot"), o.long("at"), o.str("ans") ?: "", o.strList("ids"))
                "sched_ok" -> Message.SchedOk(o.int("date"), o.str("gen") ?: "", o.int("n"))
                "steps" -> Message.Steps(o.arr("days").map { Message.StepDay(it.int("date"), it.int("steps")) })
                "reply" -> Message.Reply(o.int("id"), o.int("idx"))
                "dismiss" -> Message.Dismiss(o.int("id"))
                "recs" -> Message.Recs(o.arr("files").map { f ->
                    Message.RecFile(f.str("n") ?: "", f.long("s"), (f["marks"] as? JsonArray)?.mapNotNull { it.jsonPrimitive.longOrNull } ?: emptyList())
                })
                "wifi" -> Message.Wifi(o.str("ssid") ?: "", o.str("pass") ?: "", o.str("ip") ?: "", o.str("err"), o.str("token") ?: "")
                "ptt" -> Message.Ptt(o.str("side") ?: "me", o.bool("on"), if (o.containsKey("rate")) o.int("rate") else 16000, o.str("codec") ?: "ima-adpcm", if (o.containsKey("blocks")) o.int("blocks") else -1,
                    (o["ctx"] as? JsonObject)?.let { c -> Message.PttCtx(c.long("ci_seq"), c.int("slot"), c.int("date")) })
                "practice" -> Message.Practice(o.arr("sessions").map {
                    Message.PracticeSession(it.int("date"), it.str("start") ?: "", it.int("min"), it.str("tool") ?: "", it.int("bpm_max"), it.int("changes"))
                })
                "find_phone" -> Message.FindPhone(o.bool("on"))
                "music_cmd" -> Message.MusicCmd(o.str("cmd") ?: "")
                "apps" -> Message.Apps(o.strList("list"))
                "app_ok" -> Message.AppOk(o.str("name") ?: "")
                "err" -> Message.Err(o.str("msg") ?: "", o.str("re") ?: "")
                else -> Message.Unknown(t)
            }
        } catch (e: Exception) {
            null
        }
    }

    fun decodeBytes(b: ByteArray): Message? = decode(String(b, Charsets.UTF_8))

    private fun JsonObject.str(k: String): String? = (this[k] as? JsonPrimitive)?.takeIf { it.isString }?.content
    private fun JsonObject.long(k: String): Long = this[k]?.jsonPrimitive?.longOrNull ?: 0L
    private fun JsonObject.int(k: String): Int = this[k]?.jsonPrimitive?.intOrNull ?: 0
    private fun JsonObject.bool(k: String): Boolean = this[k]?.jsonPrimitive?.booleanOrNull ?: false
    private fun JsonObject.strList(k: String): List<String> =
        (this[k] as? JsonArray)?.mapNotNull { (it as? JsonPrimitive)?.content } ?: emptyList()
    private fun JsonObject.arr(k: String): List<JsonObject> = (this[k] as? JsonArray)?.mapNotNull { it as? JsonObject } ?: emptyList()
}
