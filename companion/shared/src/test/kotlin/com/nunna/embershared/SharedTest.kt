package com.nunna.embershared

import org.junit.Assert.*
import org.junit.Test
import javax.crypto.KeyGenerator

class SharedTest {
    private fun req(kind: RequestKind, url: String, method: String = "GET") = GatewayRequest(kind, method, url)

    @Test fun watchFetchIsAutomaticOnlyForTheHotspot() {
        val url = GatewayPolicy.watchUrl("192.168.4.1", "M_20261114_093000.wav", "0123456789abcdef")!!
        assertEquals("http://192.168.4.1/f/M_20261114_093000.wav?k=0123456789abcdef", url)
        assertTrue(GatewayPolicy.classify(req(RequestKind.WATCH_FETCH, url)) is Decision.Auto)
        // anything that is not exactly the hotspot gets refused
        assertNull(GatewayPolicy.watchUrl("10.0.0.5", "a.wav", "0123456789abcdef"))
        assertNull(GatewayPolicy.watchUrl("192.168.4.1", "../etc/passwd", "0123456789abcdef"))
        assertNull(GatewayPolicy.watchUrl("192.168.4.1", "a.wav", "short"))
        assertNull(GatewayPolicy.watchUrl("192.168.4.1", "a.wav", "0123456789ABCDEFG"))
        assertTrue(GatewayPolicy.classify(req(RequestKind.WATCH_FETCH, "http://192.168.4.1.evil.com/f/a.wav?k=0123456789abcdef")) is Decision.Deny)
        assertTrue(GatewayPolicy.classify(req(RequestKind.WATCH_FETCH, "http://192.168.4.2/f/a.wav?k=0123456789abcdef")) is Decision.Deny)
        assertTrue(GatewayPolicy.classify(req(RequestKind.WATCH_FETCH, url, "POST")) is Decision.Deny)
        assertTrue(GatewayPolicy.classify(req(RequestKind.WATCH_FETCH, "http://192.168.4.1/f/a.wav")) is Decision.Deny) // no key
    }

    @Test fun modelDownloadsNeedApprovalAndAllowlist() {
        val ok = listOf(
            "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-whisper-tiny.tar.bz2",
            "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it.litertlm",
        )
        for (u in ok) assertTrue(u, GatewayPolicy.classify(req(RequestKind.MODEL, u)) is Decision.Approval)
        val bad = listOf(
            "http://github.com/k2-fsa/sherpa-onnx/releases/download/x/y", // not https
            "https://github.com/evil/repo/releases/download/x/y",
            "https://github.com.evil.com/k2-fsa/sherpa-onnx/releases/download/x/y",
            "https://huggingface.co/someone-else/model/resolve/main/x.bin",
            "https://huggingface.co/litert-community/x/blob/main/x.bin", // not a resolve link
            "https://user@github.com/k2-fsa/sherpa-onnx/releases/download/x/y",
            "https://github.com:8443/k2-fsa/sherpa-onnx/releases/download/x/y",
            "https://github.com/k2-fsa/sherpa-onnx/releases/download/x/y?token=abc",
            "https://github.com/k2-fsa/sherpa-onnx/releases/download/../../../evil/x",
            "https://example.com/model.bin",
        )
        for (u in bad) assertTrue(u, GatewayPolicy.classify(req(RequestKind.MODEL, u)) is Decision.Deny)
    }

    @Test fun anythingElseNeedsApproval() {
        assertTrue(GatewayPolicy.classify(GatewayRequest(RequestKind.HTTP, "POST", "https://example.com/api", payload = "{\"a\":1}")) is Decision.Approval)
    }

    @Test fun redirectsStayInsideTheProvider() {
        val gh = "https://github.com/k2-fsa/sherpa-onnx/releases/download/a/b"
        assertTrue(GatewayPolicy.redirectAllowed(gh, "https://objects.githubusercontent.com/x"))
        assertTrue(GatewayPolicy.redirectAllowed(gh, "https://release-assets.githubusercontent.com/x"))
        assertFalse(GatewayPolicy.redirectAllowed(gh, "https://evil.com/x"))
        assertFalse(GatewayPolicy.redirectAllowed(gh, "http://objects.githubusercontent.com/x"))
        assertFalse(GatewayPolicy.redirectAllowed(gh, "https://githubusercontent.com.evil.com/x"))
        val hf = "https://huggingface.co/litert-community/m/resolve/main/f"
        assertTrue(GatewayPolicy.redirectAllowed(hf, "https://cas-bridge.xethub.hf.co/x"))
        assertFalse(GatewayPolicy.redirectAllowed(hf, "https://hf.co.evil.com/x"))
    }

    @Test fun aesGcmRoundTripAndTamper() {
        val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        val box = AesGcmBox { key }
        val enc = box.encryptString("secret notes", "notes")
        assertFalse(String(enc).contains("secret"))
        assertEquals("secret notes", box.decryptString(enc, "notes"))
        assertNotEquals(enc.toList(), box.encryptString("secret notes", "notes").toList()) // random IV
        assertThrows(Exception::class.java) { box.decryptString(enc, "other") } // wrong context
        val bad = enc.copyOf().also { it[20] = (it[20] + 1).toByte() }
        assertThrows(Exception::class.java) { box.decrypt(bad, "notes".toByteArray()) }
        val key2 = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        assertThrows(Exception::class.java) { AesGcmBox { key2 }.decrypt(enc, "notes".toByteArray()) }
    }

    @Test fun auditLog() {
        val e = AuditEntry(1, "MODEL", "GET", "github.com", AuditLog.scrub("http://192.168.4.1/f/a.wav?k=0123456789abcdef"), 10, "AUTO")
        assertFalse(e.url.contains("0123456789abcdef"))
        var l = emptyList<AuditEntry>()
        repeat(600) { l = AuditLog.add(l, e.copy(timeMs = it.toLong())) }
        assertEquals(500, l.size)
        assertEquals(599L, l.first().timeMs)
        assertEquals(l, AuditLog.decode(AuditLog.encode(l)))
        assertEquals("github.com", AuditLog.hostOf("https://github.com/x"))
    }
}
