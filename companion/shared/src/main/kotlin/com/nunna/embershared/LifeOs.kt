package com.nunna.embershared

import java.net.URI

/** The Life OS link the owner types into Ember Gateway: https base URL on the tailnet + a per-watch device token. */
data class LifeOsConfig(val base: String, val token: String) {
    val host: String get() = URI(base).host
}

object LifeOsPolicy {
    const val TODAY_PATH = "/api/watch/today"
    const val CHECKINS_PATH = "/api/watch/checkins"
    private val TOKEN = Regex("^lw_[0-9a-f]{32}$")
    const val AMBER_PATH = "/api/watch/amber"
    const val INBOX_PATH = "/api/watch/inbox"
    const val INBOX_ACK_PATH = "/api/watch/inbox/ack"
    private val INBOX_QUERY = Regex("^after=[0-9A-Za-z:.+%-]{0,40}$")
    private val TODAY_QUERY = Regex("^date=[0-9]{4}-[0-9]{2}-[0-9]{2}&tz=-?[0-9]{1,4}$")
    private val HOST = Regex("""^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*\.ts\.net$""")

    fun validToken(t: String) = TOKEN.matches(t)

    /**
     * Returns the normalised base ("https://host.tailnet.ts.net" or "...ts.net:8410", no trailing slash) or null when
     * the URL is not allowed: https only, host ends in .ts.net, optional explicit port, no user info, no path (a single
     * "/" is fine), no query, no fragment.
     */
    fun normalizeBase(input: String): String? = runCatching {
        val s = input.trim()
        if (s.any { it.isWhitespace() || it.code < 32 }) return null
        val u = URI(s)
        if (u.scheme != "https" || u.userInfo != null || u.rawQuery != null || u.rawFragment != null) return null
        if (u.port != -1 && u.port !in 1..65535) return null
        if (!Regex("^[^:@]+(:[0-9]{1,5})?$").matches(u.rawAuthority ?: return null)) return null
        if (!(u.rawPath.isNullOrEmpty() || u.rawPath == "/")) return null
        val h = u.host?.lowercase() ?: return null
        if (!HOST.matches(h)) return null
        if (u.port == -1 || u.port == 443) "https://$h" else "https://$h:${u.port}"
    }.getOrNull()

    /** Builds the config if both parts are valid, else null. */
    fun config(url: String, token: String): LifeOsConfig? {
        val b = normalizeBase(url) ?: return null
        return if (validToken(token.trim())) LifeOsConfig(b, token.trim()) else null
    }

    fun todayUrl(base: String, date: String, tzMinutes: Int): String = "$base$TODAY_PATH?date=$date&tz=$tzMinutes"
    fun checkinsUrl(base: String): String = "$base$CHECKINS_PATH"
    fun amberUrl(base: String): String = "$base$AMBER_PATH"
    fun inboxUrl(base: String, after: String = ""): String = if (after.isEmpty()) "$base$INBOX_PATH" else "$base$INBOX_PATH?after=$after"
    fun inboxAckUrl(base: String): String = "$base$INBOX_ACK_PATH"

    /** True only for the two automatic calls, to exactly the configured host. */
    fun isAllowed(method: String, url: String, base: String): Boolean = runCatching {
        if (normalizeBase(base) != base) return false
        val u = URI(url)
        if (u.scheme != "https" || u.userInfo != null || u.rawFragment != null) return false
        val port = if (u.port == -1 || u.port == 443) "" else ":${u.port}"
        if ("https://" + (u.host ?: return false).lowercase() + port != base) return false
        val q = u.rawQuery
        when (u.rawPath) {
            TODAY_PATH -> method == "GET" && q != null && TODAY_QUERY.matches(q)
            INBOX_PATH -> method == "GET" && (q == null || INBOX_QUERY.matches(q))
            CHECKINS_PATH, AMBER_PATH, INBOX_ACK_PATH -> method == "POST" && q == null
            else -> false
        }
    }.getOrDefault(false)

    /** Redirects are never followed for Life OS calls. */
    @Suppress("UNUSED_PARAMETER")
    fun redirectAllowed(from: String, to: String): Boolean = false

    /** Turns an HTTP status into a result code; any 3xx is refused (no redirects). */
    fun codeForStatus(status: Int): String = if (status in 200..299) Contract.OK else Contract.HTTP_ERROR
}
