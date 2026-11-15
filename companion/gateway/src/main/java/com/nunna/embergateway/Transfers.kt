package com.nunna.embergateway

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import com.nunna.embershared.Contract
import com.nunna.embershared.GatewayPolicy
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import kotlin.coroutines.resume

class TransferException(val code: String, message: String) : Exception(message)

/** The only code in the whole system that opens network connections. */
object Transfers {
    private fun copy(conn: HttpURLConnection, dest: File, expected: Long, sha256: String?, onProgress: (Int) -> Unit): Long {
        val len = conn.contentLengthLong
        if (expected > 0 && len > 0 && len != expected) throw TransferException(Contract.SIZE_MISMATCH, "Server says $len bytes, expected $expected")
        val md = if (sha256 != null) MessageDigest.getInstance("SHA-256") else null
        var n = 0L
        var last = -1
        conn.inputStream.use { i ->
            dest.outputStream().use { o ->
                val buf = ByteArray(64 * 1024)
                while (true) {
                    val r = i.read(buf)
                    if (r < 0) break
                    o.write(buf, 0, r); md?.update(buf, 0, r); n += r
                    val total = if (expected > 0) expected else len
                    if (total > 0) { val p = (n * 100 / total).toInt().coerceIn(0, 100); if (p != last) { last = p; onProgress(p) } }
                }
            }
        }
        if (expected > 0 && n != expected) throw TransferException(Contract.SIZE_MISMATCH, "Got $n bytes, expected $expected")
        if (md != null) {
            val got = md.digest().joinToString("") { "%02x".format(it) }
            if (!got.equals(sha256, true)) { dest.delete(); throw TransferException(Contract.HASH_MISMATCH, "Checksum does not match") }
        }
        return n
    }

    /** HTTPS download from an allowlisted site. Redirects are followed by hand and each hop is checked against the policy. */
    suspend fun downloadModel(url: String, expected: Long, sha256: String?, dest: File, onProgress: (Int) -> Unit): Long = withContext(Dispatchers.IO) {
        var cur = url
        for (hop in 0..5) {
            val c = URL(cur).openConnection() as HttpURLConnection
            c.instanceFollowRedirects = false
            c.connectTimeout = 20_000; c.readTimeout = 30_000
            val code = c.responseCode
            if (code in 300..399) {
                val loc = c.getHeaderField("Location") ?: throw TransferException(Contract.HTTP_ERROR, "Redirect without address")
                val next = URL(URL(cur), loc).toString()
                c.disconnect()
                if (!GatewayPolicy.redirectAllowed(url, next)) throw TransferException(Contract.NOT_ALLOWED, "Redirect to a site that is not allowed")
                cur = next
                continue
            }
            if (code != 200) throw TransferException(Contract.HTTP_ERROR, "Server answered $code")
            try { return@withContext copy(c, dest, expected, sha256, onProgress) } catch (e: IOException) { dest.delete(); throw TransferException(Contract.IO_ERROR, e.message ?: "I/O error") }
            finally { c.disconnect() }
        }
        throw TransferException(Contract.HTTP_ERROR, "Too many redirects")
    }

    private fun hasWifiPermission(ctx: Context): Boolean {
        val p = if (Build.VERSION.SDK_INT >= 33) Manifest.permission.NEARBY_WIFI_DEVICES else Manifest.permission.ACCESS_FINE_LOCATION
        return ctx.checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED
    }

    /** Joins the watch hotspot (only for this app), GETs /f/name?k=token, then lets go of the network. */
    suspend fun fetchWatch(
        ctx: Context, ssid: String, pass: String, ip: String, name: String, token: String,
        expected: Long, dest: File, onProgress: (Int) -> Unit,
    ): Long {
        val url = GatewayPolicy.watchUrl(ip, name, token) ?: throw TransferException(Contract.NOT_ALLOWED, "Not a valid watch address")
        if (!hasWifiPermission(ctx)) throw TransferException(Contract.WIFI_JOIN, "Wi-Fi permission is missing in Ember Gateway")
        val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val spec = WifiNetworkSpecifier.Builder().setSsid(ssid).setWpa2Passphrase(pass).build()
        val req = NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).setNetworkSpecifier(spec).build()
        var cb: ConnectivityManager.NetworkCallback? = null
        try {
            val net: Network? = suspendCancellableCoroutine { c ->
                val callback = object : ConnectivityManager.NetworkCallback() {
                    override fun onAvailable(network: Network) { if (c.isActive) c.resume(network) }
                    override fun onUnavailable() { if (c.isActive) c.resume(null) }
                }
                cb = callback
                cm.requestNetwork(req, callback, 30_000)
                c.invokeOnCancellation { runCatching { cm.unregisterNetworkCallback(callback) } }
            }
            if (net == null) throw TransferException(Contract.WIFI_JOIN, "Could not join the watch Wi-Fi")
            return withContext(Dispatchers.IO) {
                val c = net.openConnection(URL(url)) as HttpURLConnection
                c.instanceFollowRedirects = false
                c.connectTimeout = 10_000; c.readTimeout = 20_000
                try {
                    when (c.responseCode) {
                        200 -> {}
                        403 -> throw TransferException(Contract.WATCH_REFUSED, "The watch refused the key")
                        else -> throw TransferException(Contract.HTTP_ERROR, "Watch answered ${c.responseCode}")
                    }
                    copy(c, dest, expected, null, onProgress)
                } catch (e: IOException) {
                    dest.delete(); throw TransferException(Contract.IO_ERROR, e.message ?: "I/O error")
                } finally { c.disconnect() }
            }
        } finally {
            cb?.let { runCatching { cm.unregisterNetworkCallback(it) } }
        }
    }

    /** "Other" requests: sent only after the user approved the exact payload. Response body is saved to [dest]. */
    suspend fun http(method: String, url: String, body: String, dest: File): Long = withContext(Dispatchers.IO) {
        val c = URL(url).openConnection() as HttpURLConnection
        c.instanceFollowRedirects = false
        c.requestMethod = method
        c.connectTimeout = 20_000; c.readTimeout = 30_000
        if (body.isNotEmpty()) { c.doOutput = true; c.outputStream.use { it.write(body.toByteArray()) } }
        val code = c.responseCode
        val stream = if (code >= 400) c.errorStream else c.inputStream
        var n = 0L
        stream?.use { i -> dest.outputStream().use { o -> n = i.copyTo(o) } }
        c.disconnect()
        if (code >= 400) throw TransferException(Contract.HTTP_ERROR, "Server answered $code")
        n
    }
}
