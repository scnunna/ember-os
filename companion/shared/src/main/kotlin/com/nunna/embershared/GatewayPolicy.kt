package com.nunna.embershared

import java.net.URI

enum class RequestKind { WATCH_FETCH, MODEL, HTTP }

/** What a client asks the gateway to do. [declaredSize] and [payload] are shown to the user exactly as given. */
data class GatewayRequest(
    val kind: RequestKind,
    val method: String,
    val url: String,
    val declaredSize: Long = -1,
    val payload: String = "",
    val label: String = "",
)

sealed interface Decision {
    /** Runs without asking (still logged). */
    data class Auto(val reason: String) : Decision
    /** Needs the user to tap Approve on the approval screen. */
    data class Approval(val reason: String) : Decision
    data class Deny(val code: String, val reason: String) : Decision
}

/**
 * The gateway's rules, kept pure so they can be tested without a phone.
 *  (a) GET to the watch hotspot with its access token: automatic.
 *  (b) model downloads only from an allowlist, and every one needs approval (exact URL + size shown).
 *  (c) anything else: approval screen with the full payload.
 */
object GatewayPolicy {
    const val WATCH_IP = "192.168.4.1"
    private val NAME = Regex("^[A-Za-z0-9_.-]{1,64}$")
    private val TOKEN = Regex("^[0-9a-f]{16}$")

    /** (host, required path prefix) pairs a model download may use. */
    val MODEL_ALLOWLIST = listOf(
        "github.com" to "/k2-fsa/sherpa-onnx/releases/download/",
        "huggingface.co" to "/litert-community/",
    )

    /** Builds the one URL the watch fetch is allowed to use, or null if the inputs are not safe. */
    fun watchUrl(ip: String, name: String, token: String): String? {
        if (ip != WATCH_IP || !NAME.matches(name) || name.contains("..") || !TOKEN.matches(token)) return null
        return "http://$WATCH_IP/f/$name?k=$token"
    }

    fun isWatchUrl(url: String): Boolean = runCatching {
        val u = URI(url)
        u.scheme == "http" && u.host == WATCH_IP && (u.port == -1 || u.port == 80) && u.userInfo == null &&
            (u.path.startsWith("/f/") || u.path == "/api/files") && (u.query ?: "").contains("k=")
    }.getOrDefault(false)

    fun isAllowedModelUrl(url: String): Boolean = runCatching {
        val u = URI(url)
        if (u.scheme != "https" || u.userInfo != null || (u.port != -1 && u.port != 443) || u.fragment != null) return false
        val q = u.query
        if (q != null && q != "download=true") return false
        if (u.path.contains("..") || u.rawPath.contains("%")) return false
        MODEL_ALLOWLIST.any { (host, prefix) ->
            u.host == host && u.path.startsWith(prefix) && (host != "huggingface.co" || u.path.contains("/resolve/"))
        }
    }.getOrDefault(false)

    /** Redirects of an approved download may only stay inside the same provider's own download hosts, over https. */
    fun redirectAllowed(from: String, to: String): Boolean = runCatching {
        val f = URI(from); val t = URI(to)
        if (t.scheme != "https" || t.userInfo != null || (t.port != -1 && t.port != 443)) return false
        val th = t.host ?: return false
        when (f.host) {
            "github.com" -> th == "github.com" || th.endsWith(".githubusercontent.com")
            "huggingface.co" -> th == "huggingface.co" || th.endsWith(".hf.co") || th.endsWith(".huggingface.co")
            else -> th == f.host
        }
    }.getOrDefault(false)

    fun classify(r: GatewayRequest): Decision = when (r.kind) {
        RequestKind.WATCH_FETCH ->
            if (r.method == "GET" && isWatchUrl(r.url)) Decision.Auto("Download from your watch (its own Wi-Fi, with its one-time key)")
            else Decision.Deny(Contract.NOT_ALLOWED, "Only GET requests to the watch Wi-Fi are automatic.")
        RequestKind.MODEL ->
            if (r.method == "GET" && isAllowedModelUrl(r.url)) Decision.Approval("Model download from an approved site")
            else Decision.Deny(Contract.NOT_ALLOWED, "Model downloads are only allowed from github.com/k2-fsa and huggingface.co/litert-community.")
        RequestKind.HTTP -> Decision.Approval("Other request: check the address and the data carefully")
    }
}
