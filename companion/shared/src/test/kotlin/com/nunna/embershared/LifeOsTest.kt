package com.nunna.embershared

import org.junit.Assert.*
import org.junit.Test

class LifeOsTest {
    private val base = "https://laptop.tail1234.ts.net"
    private val tok = "lw_" + "0123456789abcdef".repeat(2)
    private fun life(url: String, method: String = "GET") = GatewayRequest(RequestKind.LIFEOS, method, url)
    private fun decide(url: String, method: String = "GET", b: String? = base) = GatewayPolicy.classify(life(url, method), b)

    @Test fun onlyTheTwoCallsAreAutomatic() {
        assertTrue(decide("$base/api/watch/today?date=2026-11-15&tz=-300") is Decision.Auto)
        assertTrue(decide("$base/api/watch/today?date=2026-11-15&tz=60") is Decision.Auto)
        assertTrue(decide("$base/api/watch/checkins", "POST") is Decision.Auto)
        assertEquals("$base/api/watch/today?date=2026-11-15&tz=-300", LifeOsPolicy.todayUrl(base, "2026-11-15", -300))
        assertTrue(decide(LifeOsPolicy.todayUrl(base, "2026-11-15", -300)) is Decision.Auto)
        assertTrue(decide(LifeOsPolicy.checkinsUrl(base), "POST") is Decision.Auto)
    }

    @Test fun handoffLinkFromLifeOs() {
        val b = java.net.URLEncoder.encode("https://lifeos.example-tailnet.ts.net:8410", "UTF-8")
        val ok = LifeOsPolicy.parseHandoff("embergateway://lifeos-link?base=$b&token=$tok")
        assertEquals("https://lifeos.example-tailnet.ts.net:8410", ok!!.base)
        assertEquals(tok, ok.token)
        assertNotNull(LifeOsPolicy.parseHandoff("embergateway://lifeos-link?token=$tok&base=$b"))
        for (bad in listOf(
            "embergateway://lifeos-link?base=${java.net.URLEncoder.encode("https://evil.example.com", "UTF-8")}&token=$tok",
            "embergateway://lifeos-link?base=${java.net.URLEncoder.encode("http://lifeos.example-tailnet.ts.net", "UTF-8")}&token=$tok",
            "embergateway://lifeos-link?base=$b&token=lw_short",
            "embergateway://lifeos-link?base=$b&token=$tok&extra=1",
            "embergateway://lifeos-link?base=$b",
            "embergateway://other?base=$b&token=$tok",
            "https://lifeos-link?base=$b&token=$tok",
            "embergateway://lifeos-link/x?base=$b&token=$tok",
            "embergateway://lifeos-link?base=$b&token=$tok#f",
            "not a uri",
        )) assertNull(bad, LifeOsPolicy.parseHandoff(bad))
    }

    @Test fun otherHostsAreDenied() {
        for (u in listOf(
            "https://evil.example.com/api/watch/checkins", "https://other.tail1234.ts.net/api/watch/checkins",
            "https://laptop.tail1234.ts.net.evil.com/api/watch/checkins", "https://evil.com/laptop.tail1234.ts.net/api/watch/checkins",
        )) assertTrue(u, decide(u, "POST") is Decision.Deny)
    }

    @Test fun otherPathsAreDenied() {
        for (u in listOf(
            "$base/", "$base/api/watch/devices", "$base/api/watch/today/x?date=2026-11-15&tz=0", "$base/api/watch/checkins/1",
            "$base/api/other", "$base/../etc/passwd", "$base/api/watch/%74oday?date=2026-11-15&tz=0",
        )) assertTrue(u, decide(u) is Decision.Deny)
    }

    @Test fun httpUserInfoPortFragmentDenied() {
        assertTrue(decide("http://laptop.tail1234.ts.net/api/watch/checkins", "POST") is Decision.Deny)
        assertTrue(decide("https://user:pw@laptop.tail1234.ts.net/api/watch/checkins", "POST") is Decision.Deny)
        assertTrue(decide("https://laptop.tail1234.ts.net:8443/api/watch/checkins", "POST") is Decision.Deny)
        assertTrue(decide("$base/api/watch/checkins#x", "POST") is Decision.Deny)
    }

    @Test fun queriesAreChecked() {
        assertTrue(decide("$base/api/watch/today") is Decision.Deny)
        assertTrue(decide("$base/api/watch/today?date=2026-11-15&tz=0&x=1") is Decision.Deny)
        assertTrue(decide("$base/api/watch/today?date=2026-11-15&tz=abc") is Decision.Deny)
        assertTrue(decide("$base/api/watch/today?tz=0&date=2026-11-15") is Decision.Deny)
        assertTrue(decide("$base/api/watch/checkins?a=b", "POST") is Decision.Deny)
    }

    @Test fun methodsAreChecked() {
        assertTrue(decide("$base/api/watch/checkins", "GET") is Decision.Deny)
        assertTrue(decide("$base/api/watch/today?date=2026-11-15&tz=0", "POST") is Decision.Deny)
        assertTrue(decide("$base/api/watch/checkins", "DELETE") is Decision.Deny)
    }

    @Test fun notConfiguredIsReported() {
        val d = decide("$base/api/watch/checkins", "POST", null)
        assertEquals(Contract.NOT_CONFIGURED, (d as Decision.Deny).code)
    }

    @Test fun linkSettingValidation() {
        assertEquals(base, LifeOsPolicy.normalizeBase(base))
        assertEquals(base, LifeOsPolicy.normalizeBase("  https://Laptop.Tail1234.ts.net/ "))
        assertEquals(base, LifeOsPolicy.normalizeBase("https://laptop.tail1234.ts.net:443"))
        for (bad in listOf(
            "http://laptop.tail1234.ts.net", "https://laptop.example.com", "https://ts.net", "https://evilts.net",
            "https://laptop.tail1234.ts.net.evil.com", "https://u:p@laptop.tail1234.ts.net", "https://laptop.tail1234.ts.net/app",
            "https://laptop.tail1234.ts.net/?a=1", "https://laptop.tail1234.ts.net#x", "https://laptop.tail1234.ts.net:0", "https://laptop.tail1234.ts.net:99999", "https://laptop.tail1234.ts.net:",
            "https://192.168.1.5", "ftp://laptop.tail1234.ts.net", "", "laptop.tail1234.ts.net", "https://lap top.tail1234.ts.net",
        )) assertNull(bad, LifeOsPolicy.normalizeBase(bad))
        assertEquals("laptop.tail1234.ts.net", LifeOsPolicy.config(base, tok)!!.host)
        assertNull(LifeOsPolicy.config("http://laptop.tail1234.ts.net", tok))
    }

    @Test fun badTokensRejected() {
        for (t in listOf(
            "", "lw_", "lw_0123", "LW_" + "0".repeat(32), "lw_" + "G".repeat(32), "lw_" + "A".repeat(32),
            "lw_" + "0".repeat(33), "lw_" + "0".repeat(31), "xx_" + "0".repeat(32), "lw_" + "0".repeat(32) + "x",
        )) assertNull(t, LifeOsPolicy.config(base, t))
        assertTrue(LifeOsPolicy.validToken(tok))
    }

    @Test fun redirectsAreRefused() {
        assertFalse(LifeOsPolicy.redirectAllowed("$base/api/watch/checkins", "$base/api/watch/checkins"))
        assertFalse(LifeOsPolicy.redirectAllowed("$base/api/watch/checkins", "https://evil.com/"))
        assertEquals(Contract.OK, LifeOsPolicy.codeForStatus(200))
        for (st in listOf(301, 302, 307, 308, 401, 404, 429, 500)) assertEquals(Contract.HTTP_ERROR, LifeOsPolicy.codeForStatus(st))
    }

    @Test fun contractCodes() {
        assertEquals(listOf(5, 6, 7), listOf(Contract.MSG_LIFEOS_TODAY, Contract.MSG_LIFEOS_CHECKINS, Contract.MSG_LIFEOS_STATUS))
        assertEquals(listOf("NOT_CONFIGURED", "HTTP_ERROR", "OFFLINE"), listOf(Contract.NOT_CONFIGURED, Contract.HTTP_ERROR, Contract.OFFLINE))
    }

    private val pbase = "https://lifeos.example-tailnet.ts.net:8410"

    @Test fun portIsAllowedAndPartOfTheHost() {
        assertEquals(pbase, LifeOsPolicy.normalizeBase(pbase))
        assertEquals(pbase, LifeOsPolicy.normalizeBase("$pbase/"))
        assertEquals("lifeos.example-tailnet.ts.net", LifeOsPolicy.config(pbase, tok)!!.host)
        assertNull(LifeOsPolicy.normalizeBase("https://evil.example.com:8410"))
        assertNull(LifeOsPolicy.normalizeBase("https://user@lifeos.example-tailnet.ts.net:8410"))
        assertNull(LifeOsPolicy.normalizeBase("$pbase/app"))
        assertTrue(decide(LifeOsPolicy.checkinsUrl(pbase), "POST", pbase) is Decision.Auto)
        assertTrue(decide(LifeOsPolicy.todayUrl(pbase, "2026-11-15", -300), "GET", pbase) is Decision.Auto)
        // the same host on another port, or no port, is a different server
        assertTrue(decide("https://lifeos.example-tailnet.ts.net:8411/api/watch/checkins", "POST", pbase) is Decision.Deny)
        assertTrue(decide("https://lifeos.example-tailnet.ts.net/api/watch/checkins", "POST", pbase) is Decision.Deny)
        assertTrue(decide("https://lifeos.example-tailnet.ts.net:8410/api/watch/checkins", "POST", base) is Decision.Deny)
        assertTrue(decide("http://lifeos.example-tailnet.ts.net:8410/api/watch/checkins", "POST", pbase) is Decision.Deny)
    }

    @Test fun amberRoutes() {
        assertTrue(decide(LifeOsPolicy.amberUrl(pbase), "POST", pbase) is Decision.Auto)
        assertTrue(decide(LifeOsPolicy.inboxUrl(pbase), "GET", pbase) is Decision.Auto)
        assertTrue(decide(LifeOsPolicy.inboxUrl(pbase, "2026-11-15T10:00:00Z"), "GET", pbase) is Decision.Auto)
        assertTrue(decide(LifeOsPolicy.inboxUrl(pbase, "2026-11-15T10%3A00%3A00Z"), "GET", pbase) is Decision.Auto)
        assertTrue(decide(LifeOsPolicy.inboxAckUrl(pbase), "POST", pbase) is Decision.Auto)
        // wrong methods
        assertTrue(decide(LifeOsPolicy.amberUrl(pbase), "GET", pbase) is Decision.Deny)
        assertTrue(decide(LifeOsPolicy.inboxUrl(pbase), "POST", pbase) is Decision.Deny)
        assertTrue(decide(LifeOsPolicy.inboxAckUrl(pbase), "GET", pbase) is Decision.Deny)
        // wrong queries / paths
        assertTrue(decide("$pbase/api/watch/amber?x=1", "POST", pbase) is Decision.Deny)
        assertTrue(decide("$pbase/api/watch/inbox?after=1&x=2", "GET", pbase) is Decision.Deny)
        assertTrue(decide("$pbase/api/watch/inbox?x=2", "GET", pbase) is Decision.Deny)
        assertTrue(decide("$pbase/api/watch/inbox?after=a b", "GET", pbase) is Decision.Deny)
        assertTrue(decide("$pbase/api/watch/inbox/ack?a=1", "POST", pbase) is Decision.Deny)
        assertTrue(decide("$pbase/api/watch/inbox/other", "POST", pbase) is Decision.Deny)
        assertTrue(decide("$pbase/api/watch/amber/x", "POST", pbase) is Decision.Deny)
        assertTrue(decide("$pbase/api/amber", "POST", pbase) is Decision.Deny)
        assertEquals(listOf(8, 9, 10), listOf(Contract.MSG_LIFEOS_AMBER, Contract.MSG_LIFEOS_INBOX, Contract.MSG_LIFEOS_INBOX_ACK))
    }
}
