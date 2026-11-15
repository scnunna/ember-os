package com.nunna.embercompanion.android

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.ParcelFileDescriptor
import com.nunna.embercompanion.link.AmberApi
import com.nunna.embercompanion.link.LifeOsApi
import com.nunna.embercompanion.link.LifeOsResult
import com.nunna.embercompanion.link.LifeOsStatus
import com.nunna.embershared.Contract
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.withTimeoutOrNull
import java.io.InputStream
import java.util.UUID

data class GatewayResult(val ok: Boolean, val code: String, val message: String, val uri: Uri?, val bytes: Long, val extra: Bundle? = null) {
    /** Plain sentence for the user, with a code. */
    fun userMessage(): String = when (code) {
        Contract.OK -> ""
        Contract.DENIED -> Errors.GW_NO
        Contract.EXPIRED -> Errors.GW_EXPIRED
        Contract.NOT_ALLOWED -> Errors.GW_BLOCKED
        Contract.WIFI_JOIN -> Errors.WIFI_JOIN
        Contract.WATCH_REFUSED -> Errors.WIFI_403
        Contract.SIZE_MISMATCH -> Errors.WIFI_SIZE
        Contract.HASH_MISMATCH -> Errors.GW_HASH
        "MISSING" -> Errors.GW_MISSING
        "SECURITY" -> Errors.GW_DENIED
        else -> Errors.GW_FAIL
    }
}

/** Where downloads come from. The real one talks to Ember Gateway; tests use a fake. */
interface GatewayApi {
    suspend fun fetchWatchFile(ssid: String, pass: String, ip: String, name: String, token: String, expectedSize: Long, label: String, onProgress: (Int) -> Unit, onNeedsApproval: () -> Unit): GatewayResult
    suspend fun downloadModel(url: String, expectedSize: Long, sha256: String?, label: String, onProgress: (Int) -> Unit, onNeedsApproval: () -> Unit): GatewayResult
    fun open(uri: Uri): InputStream?
    fun delete(uri: Uri)
}

/**
 * Talks to Ember Gateway over a bound service (Messenger). This app has no internet permission, so every download
 * goes through the Gateway; the signature permission means only apps signed with the same key can do this.
 */
class GatewayClient(private val ctx: Context) : GatewayApi, LifeOsApi, AmberApi {
    fun installed(): Boolean = runCatching { ctx.packageManager.getPackageInfo(Contract.GATEWAY_PACKAGE, 0); true }.getOrDefault(false)

    fun approvalsIntent(): Intent = Intent().setClassName(Contract.GATEWAY_PACKAGE, Contract.APPROVALS_ACTIVITY).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    private suspend fun call(what: Int, data: Bundle, onProgress: (Int) -> Unit, onNeedsApproval: () -> Unit): GatewayResult {
        if (!installed()) return GatewayResult(false, "MISSING", "", null, 0)
        val id = UUID.randomUUID().toString()
        data.putString(Contract.K_ID, id)
        val done = CompletableDeferred<GatewayResult>()
        val handler = Handler(Looper.getMainLooper()) { m ->
            val b = m.data
            if (b.getString(Contract.K_ID) == id) when (m.what) {
                Contract.MSG_PROGRESS -> onProgress(b.getInt(Contract.K_PERCENT))
                Contract.MSG_NEEDS_APPROVAL -> onNeedsApproval()
                Contract.MSG_RESULT -> done.complete(
                    GatewayResult(b.getBoolean(Contract.K_OK), b.getString(Contract.K_CODE) ?: Contract.IO_ERROR, b.getString(Contract.K_MESSAGE) ?: "",
                        b.getString(Contract.K_URI)?.takeIf { it.isNotEmpty() }?.let { Uri.parse(it) }, b.getLong(Contract.K_BYTES), Bundle(b)),
                )
            }
            true
        }
        val reply = Messenger(handler)
        val connected = CompletableDeferred<Messenger>()
        val conn = object : ServiceConnection {
            override fun onServiceConnected(n: ComponentName, s: IBinder) { connected.complete(Messenger(s)) }
            override fun onServiceDisconnected(n: ComponentName) { done.complete(GatewayResult(false, Contract.IO_ERROR, "Gateway stopped", null, 0)) }
        }
        val intent = Intent().setClassName(Contract.GATEWAY_PACKAGE, Contract.SERVICE_CLASS)
        val bound = try { ctx.bindService(intent, conn, Context.BIND_AUTO_CREATE) } catch (e: SecurityException) { return GatewayResult(false, "SECURITY", "", null, 0) }
        if (!bound) return GatewayResult(false, "SECURITY", "", null, 0)
        var server: Messenger? = null
        try {
            server = withTimeoutOrNull(10_000) { connected.await() } ?: return GatewayResult(false, Contract.IO_ERROR, "Gateway did not start", null, 0)
            server.send(Message.obtain(null, what).also { it.data = data; it.replyTo = reply })
            // a Gateway that never answers must not block the caller forever (downloads report progress meanwhile)
            return withTimeoutOrNull(if (what == Contract.MSG_DOWNLOAD_MODEL || what == Contract.MSG_FETCH_WATCH) 30 * 60_000L else 90_000L) { done.await() }
                ?: GatewayResult(false, Contract.IO_ERROR, "Gateway did not answer", null, 0)
        } catch (e: CancellationException) {
            runCatching { server?.send(Message.obtain(null, Contract.MSG_CANCEL).also { it.data = Bundle().apply { putString(Contract.K_ID, id) } }) }
            throw e
        } finally {
            runCatching { ctx.unbindService(conn) }
        }
    }

    override suspend fun fetchWatchFile(ssid: String, pass: String, ip: String, name: String, token: String, expectedSize: Long, label: String, onProgress: (Int) -> Unit, onNeedsApproval: () -> Unit) =
        call(Contract.MSG_FETCH_WATCH, Bundle().apply {
            putString(Contract.K_SSID, ssid); putString(Contract.K_PASS, pass); putString(Contract.K_IP, ip); putString(Contract.K_NAME, name)
            putString(Contract.K_TOKEN, token); putLong(Contract.K_SIZE, expectedSize); putString(Contract.K_LABEL, label)
        }, onProgress, onNeedsApproval)

    override suspend fun downloadModel(url: String, expectedSize: Long, sha256: String?, label: String, onProgress: (Int) -> Unit, onNeedsApproval: () -> Unit) =
        call(Contract.MSG_DOWNLOAD_MODEL, Bundle().apply {
            putString(Contract.K_URL, url); putLong(Contract.K_SIZE, expectedSize); putString(Contract.K_LABEL, label)
            if (sha256 != null) putString(Contract.K_SHA256, sha256)
        }, onProgress, onNeedsApproval)

    private fun lifeResult(r: GatewayResult): LifeOsResult {
        val b = r.extra
        return LifeOsResult(r.ok, r.code, b?.getInt(Contract.K_STATUS) ?: 0, b?.getString(Contract.K_BODY) ?: "", b?.getLong(Contract.K_UPTO, -1L) ?: -1L, r.message)
    }

    private fun lifeFail(r: GatewayResult) = when (r.code) { "MISSING", "SECURITY" -> LifeOsResult(false, Contract.NOT_CONFIGURED, message = r.code); else -> lifeResult(r) }

    override suspend fun today(dateIso: String, tzMinutes: Int): LifeOsResult {
        val r = call(Contract.MSG_LIFEOS_TODAY, Bundle().apply { putString(Contract.K_DATE, dateIso); putInt(Contract.K_TZ, tzMinutes) }, {}, {})
        return if (r.ok) lifeResult(r) else lifeFail(r)
    }

    override suspend fun checkins(bodyJson: String): LifeOsResult {
        val r = call(Contract.MSG_LIFEOS_CHECKINS, Bundle().apply { putString(Contract.K_BODY, bodyJson) }, {}, {})
        return if (r.ok) lifeResult(r) else lifeFail(r)
    }

    override suspend fun send(bodyJson: String): LifeOsResult {
        val r = call(Contract.MSG_LIFEOS_AMBER, Bundle().apply { putString(Contract.K_BODY, bodyJson) }, {}, {})
        return if (r.ok) lifeResult(r) else lifeFail(r)
    }

    override suspend fun inbox(): LifeOsResult {
        val r = call(Contract.MSG_LIFEOS_INBOX, Bundle(), {}, {})
        return if (r.ok) lifeResult(r) else lifeFail(r)
    }

    override suspend fun inboxAck(bodyJson: String): LifeOsResult {
        val r = call(Contract.MSG_LIFEOS_INBOX_ACK, Bundle().apply { putString(Contract.K_BODY, bodyJson) }, {}, {})
        return if (r.ok) lifeResult(r) else lifeFail(r)
    }

    override suspend fun status(): LifeOsStatus {
        if (!installed()) return LifeOsStatus(false, false)
        val r = call(Contract.MSG_LIFEOS_STATUS, Bundle(), {}, {})
        return LifeOsStatus(true, r.ok && r.extra?.getBoolean(Contract.K_CONFIGURED) == true, r.extra?.getString(Contract.K_HOST) ?: "")
    }

    /** Opens Ember Gateway on its Life OS tab. */
    fun openLifeOsSettings(ctx: Context) { runCatching { ctx.startActivity(approvalsIntent().putExtra("tab", 2)) } }

    override fun open(uri: Uri): InputStream? = runCatching {
        ParcelFileDescriptor.AutoCloseInputStream(ctx.contentResolver.openFileDescriptor(uri, "r"))
    }.getOrNull()

    override fun delete(uri: Uri) { runCatching { ctx.contentResolver.delete(uri, null, null) } }

    fun openApprovals(ctx: Context) { runCatching { ctx.startActivity(approvalsIntent()) } }
}
