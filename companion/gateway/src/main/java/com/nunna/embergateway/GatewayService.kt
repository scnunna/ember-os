package com.nunna.embergateway

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import com.nunna.embershared.AuditEntry
import com.nunna.embershared.AuditLog
import com.nunna.embershared.Contract
import com.nunna.embershared.Decision
import com.nunna.embershared.GatewayPolicy
import com.nunna.embershared.GatewayRequest
import com.nunna.embershared.LifeOsPolicy
import com.nunna.embershared.RequestKind
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import java.io.File
import java.util.UUID

/**
 * Bound service (Messenger). Protected by a signature permission, so only Ember Companion can bind.
 * Every request is classified by [GatewayPolicy], logged, and either run, sent to the approval screen, or refused.
 */
class GatewayService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default + kotlinx.coroutines.CoroutineExceptionHandler { _, e ->
        android.util.Log.e("EmberGateway", "request failed", e)
        runCatching { GatewayStore.crash.record("CAUGHT", e) }
    })
    private val jobs = HashMap<String, Job>()
    private lateinit var messenger: Messenger

    override fun onCreate() {
        super.onCreate()
        GatewayStore.init(this)
        messenger = Messenger(Handler(Looper.getMainLooper()) { handle(it); true })
    }

    override fun onBind(intent: Intent?): IBinder = messenger.binder

    override fun onDestroy() { scope.coroutineContext[Job]?.cancel(); super.onDestroy() }

    private fun reply(to: Messenger?, what: Int, b: Bundle) {
        try { to?.send(Message.obtain(null, what).also { it.data = b }) } catch (_: Exception) {}
    }

    private fun handle(m: Message) {
        val b = m.data
        val to = m.replyTo
        val id = b.getString(Contract.K_ID) ?: return
        when (m.what) {
            Contract.MSG_CANCEL -> { jobs[id]?.cancel(); return }
            Contract.MSG_LIFEOS_STATUS -> {
                val c = GatewayStore.lifeOs.value
                reply(to, Contract.MSG_RESULT, Bundle().apply {
                    putString(Contract.K_ID, id); putBoolean(Contract.K_OK, true); putString(Contract.K_CODE, Contract.OK)
                    putBoolean(Contract.K_CONFIGURED, c != null); putString(Contract.K_HOST, c?.host ?: "")
                })
            }
            Contract.MSG_LIFEOS_TODAY, Contract.MSG_LIFEOS_CHECKINS, Contract.MSG_LIFEOS_AMBER, Contract.MSG_LIFEOS_INBOX, Contract.MSG_LIFEOS_INBOX_ACK -> {
                jobs[id] = scope.launch { runLifeOs(m.what, id, b, to); jobs.remove(id) }
            }
            Contract.MSG_FETCH_WATCH, Contract.MSG_DOWNLOAD_MODEL, Contract.MSG_HTTP -> {
                jobs[id] = scope.launch { run(m.what, id, b, to); jobs.remove(id) }
            }
        }
    }

    private fun result(to: Messenger?, id: String, code: String, message: String = "", uri: String = "", bytes: Long = 0) {
        reply(to, Contract.MSG_RESULT, Bundle().apply {
            putString(Contract.K_ID, id); putBoolean(Contract.K_OK, code == Contract.OK); putString(Contract.K_CODE, code)
            putString(Contract.K_MESSAGE, message); putString(Contract.K_URI, uri); putLong(Contract.K_BYTES, bytes)
        })
    }

    private fun lifeOsResult(to: Messenger?, id: String, code: String, message: String = "", status: Int = 0, body: String = "", upto: Long = -1) {
        reply(to, Contract.MSG_RESULT, Bundle().apply {
            putString(Contract.K_ID, id); putBoolean(Contract.K_OK, code == Contract.OK); putString(Contract.K_CODE, code)
            putString(Contract.K_MESSAGE, message); putInt(Contract.K_STATUS, status); putString(Contract.K_BODY, body); putLong(Contract.K_UPTO, upto)
        })
    }

    /** Life OS calls (today, checkins, amber, inbox, inbox ack): classified by [GatewayPolicy], logged, no redirects, never needs a tap. */
    private suspend fun runLifeOs(what: Int, id: String, b: Bundle, to: Messenger?) {
        val cfg = GatewayStore.lifeOs.value
        if (cfg == null) { lifeOsResult(to, id, Contract.NOT_CONFIGURED, "Set up the Life OS link in Ember Gateway"); return }
        val body = b.getString(Contract.K_BODY)
        val method: String; val url: String; val sendBody: String?
        when (what) {
            Contract.MSG_LIFEOS_TODAY -> { method = "GET"; url = LifeOsPolicy.todayUrl(cfg.base, b.getString(Contract.K_DATE) ?: "", b.getInt(Contract.K_TZ, 0)); sendBody = null }
            Contract.MSG_LIFEOS_INBOX -> { method = "GET"; url = LifeOsPolicy.inboxUrl(cfg.base, b.getString(Contract.K_AFTER) ?: ""); sendBody = null }
            Contract.MSG_LIFEOS_CHECKINS -> { method = "POST"; url = LifeOsPolicy.checkinsUrl(cfg.base); sendBody = body ?: "" }
            Contract.MSG_LIFEOS_AMBER -> { method = "POST"; url = LifeOsPolicy.amberUrl(cfg.base); sendBody = body ?: "" }
            else -> { method = "POST"; url = LifeOsPolicy.inboxAckUrl(cfg.base); sendBody = body ?: "" }
        }
        // The audit log shows sizes, never the text the owner said.
        val req = GatewayRequest(RequestKind.LIFEOS, method, url, -1, if (sendBody == null) "" else "(${sendBody.length} bytes)", "Life OS")
        fun entry(outcome: String, note: String = "") = AuditEntry(System.currentTimeMillis(), req.kind.name, method, AuditLog.hostOf(url), url, 0, outcome, note)
        val d = GatewayPolicy.classify(req, cfg.base)
        if (d !is Decision.Auto) {
            val reason = (d as? Decision.Deny)?.reason ?: "Not allowed"
            GatewayStore.log(entry("BLOCKED", reason)); lifeOsResult(to, id, Contract.NOT_ALLOWED, reason); return
        }
        if (sendBody != null && (sendBody.isEmpty() || sendBody.length > 64 * 1024)) {
            GatewayStore.log(entry("BLOCKED", "bad body size")); lifeOsResult(to, id, Contract.NOT_ALLOWED, "Bad body"); return
        }
        try {
            val (status, text) = Transfers.lifeOs(method, url, cfg.token, sendBody)
            val code = LifeOsPolicy.codeForStatus(status)
            GatewayStore.log(entry(if (code == Contract.OK) "AUTO" else "FAILED", "HTTP $status"))
            if (code != Contract.OK) { lifeOsResult(to, id, code, "Life OS answered $status", status); return }
            var upto = -1L
            if (what == Contract.MSG_LIFEOS_CHECKINS) upto = runCatching { org.json.JSONObject(text).getLong("upto") }.getOrDefault(-1L)
            val returnBody = what == Contract.MSG_LIFEOS_TODAY || what == Contract.MSG_LIFEOS_AMBER || what == Contract.MSG_LIFEOS_INBOX
            lifeOsResult(to, id, Contract.OK, "", status, if (returnBody) text else "", upto)
        } catch (e: kotlinx.coroutines.CancellationException) { throw e
        } catch (e: java.io.IOException) {
            GatewayStore.log(entry("FAILED", "offline: " + e.javaClass.simpleName)); lifeOsResult(to, id, Contract.OFFLINE, "Could not reach Life OS")
        } catch (e: Exception) {
            GatewayStore.log(entry("FAILED", e.javaClass.simpleName)); lifeOsResult(to, id, Contract.IO_ERROR, e.message ?: "")
        }
    }

    private suspend fun run(what: Int, id: String, b: Bundle, to: Messenger?) {
        val size = b.getLong(Contract.K_SIZE, -1)
        val label = b.getString(Contract.K_LABEL) ?: ""
        val req: GatewayRequest = when (what) {
            Contract.MSG_FETCH_WATCH -> {
                val url = GatewayPolicy.watchUrl(b.getString(Contract.K_IP) ?: "", b.getString(Contract.K_NAME) ?: "", b.getString(Contract.K_TOKEN) ?: "") ?: "invalid"
                GatewayRequest(RequestKind.WATCH_FETCH, "GET", url, size, "", label.ifBlank { "Recording from watch" })
            }
            Contract.MSG_DOWNLOAD_MODEL -> GatewayRequest(RequestKind.MODEL, "GET", b.getString(Contract.K_URL) ?: "", size, "", label)
            else -> GatewayRequest(RequestKind.HTTP, b.getString(Contract.K_METHOD) ?: "GET", b.getString(Contract.K_URL) ?: "", size, b.getString(Contract.K_BODY) ?: "", label)
        }
        fun entry(outcome: String, bytes: Long = 0, note: String = "") =
            AuditEntry(System.currentTimeMillis(), req.kind.name, req.method, AuditLog.hostOf(req.url), req.url, bytes, outcome, note)

        var outcome = "AUTO"
        when (val d = GatewayPolicy.classify(req)) {
            is Decision.Deny -> { GatewayStore.log(entry("BLOCKED", note = d.reason)); result(to, id, Contract.NOT_ALLOWED, d.reason); return }
            is Decision.Approval -> {
                reply(to, Contract.MSG_NEEDS_APPROVAL, Bundle().apply { putString(Contract.K_ID, id) })
                val ok = GatewayStore.awaitApproval(PendingApproval(id, req, d.reason, System.currentTimeMillis()))
                if (ok == null) { GatewayStore.log(entry("EXPIRED")); result(to, id, Contract.EXPIRED, "Nobody approved it in time"); return }
                if (!ok) { GatewayStore.log(entry("DENIED")); result(to, id, Contract.DENIED, "You said no"); return }
                outcome = "APPROVED"
            }
            is Decision.Auto -> {}
        }
        val fileId = UUID.randomUUID().toString()
        val dest = File(GatewayStore.outDir(), fileId)
        try {
            val progress: (Int) -> Unit = { p -> reply(to, Contract.MSG_PROGRESS, Bundle().apply { putString(Contract.K_ID, id); putInt(Contract.K_PERCENT, p) }) }
            val bytes = when (what) {
                Contract.MSG_FETCH_WATCH -> Transfers.fetchWatch(
                    this, b.getString(Contract.K_SSID) ?: "", b.getString(Contract.K_PASS) ?: "", b.getString(Contract.K_IP) ?: "",
                    b.getString(Contract.K_NAME) ?: "", b.getString(Contract.K_TOKEN) ?: "", size, dest, progress,
                )
                Contract.MSG_DOWNLOAD_MODEL -> Transfers.downloadModel(req.url, size, b.getString(Contract.K_SHA256), dest, progress)
                else -> Transfers.http(req.method, req.url, req.payload, dest)
            }
            GatewayStore.log(entry(outcome, bytes))
            result(to, id, Contract.OK, uri = "content://${Contract.AUTHORITY}/out/$fileId", bytes = bytes)
        } catch (e: TransferException) {
            dest.delete(); GatewayStore.log(entry("FAILED", note = e.code)); result(to, id, e.code, e.message ?: "")
        } catch (e: kotlinx.coroutines.CancellationException) {
            dest.delete(); GatewayStore.log(entry("FAILED", note = "cancelled")); result(to, id, Contract.CANCELLED, "Cancelled"); throw e
        } catch (e: Exception) {
            dest.delete(); GatewayStore.log(entry("FAILED", note = e.javaClass.simpleName)); result(to, id, Contract.IO_ERROR, e.message ?: "")
        }
    }
}
