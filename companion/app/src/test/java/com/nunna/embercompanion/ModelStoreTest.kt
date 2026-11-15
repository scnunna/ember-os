package com.nunna.embercompanion

import android.content.Context
import android.net.Uri
import androidx.test.core.app.ApplicationProvider
import com.nunna.embercompanion.android.*
import com.nunna.embershared.Contract
import kotlinx.coroutines.runBlocking
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveOutputStream
import org.apache.commons.compress.compressors.bzip2.BZip2CompressorOutputStream
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class ModelStoreTest {
    private val ctx: Context get() = ApplicationProvider.getApplicationContext()

    private class FakeGateway(val result: GatewayResult, val data: ByteArray = ByteArray(0)) : GatewayApi {
        var requestedUrl: String? = null
        var requestedSha: String? = null
        var deleted = 0
        override suspend fun fetchWatchFile(ssid: String, pass: String, ip: String, name: String, token: String, expectedSize: Long, label: String, onProgress: (Int) -> Unit, onNeedsApproval: () -> Unit) = result
        override suspend fun downloadModel(url: String, expectedSize: Long, sha256: String?, label: String, onProgress: (Int) -> Unit, onNeedsApproval: () -> Unit): GatewayResult {
            requestedUrl = url; requestedSha = sha256; onNeedsApproval(); onProgress(50); return result
        }
        override fun open(uri: Uri): InputStream? = ByteArrayInputStream(data)
        override fun delete(uri: Uri) { deleted++ }
    }

    private fun tarBz2(files: Map<String, String>): ByteArray {
        val bo = ByteArrayOutputStream()
        TarArchiveOutputStream(BZip2CompressorOutputStream(bo)).use { t ->
            for ((n, c) in files) {
                val b = c.toByteArray()
                t.putArchiveEntry(TarArchiveEntry("sherpa-onnx-whisper-tiny/$n").also { it.size = b.size.toLong() })
                t.write(b); t.closeArchiveEntry()
            }
        }
        return bo.toByteArray()
    }

    private val ok = GatewayResult(true, Contract.OK, "", Uri.parse("content://x/out/1"), 10)

    @Test fun downloadsThroughTheGatewayAndKeepsOnlyInt8Files() = runBlocking {
        val gw = FakeGateway(ok, tarBz2(mapOf("tiny-encoder.onnx" to "big", "tiny-encoder.int8.onnx" to "e", "tiny-decoder.int8.onnx" to "d", "tiny-tokens.txt" to "t")))
        val store = ModelStore(ctx, gw, SpeechModels.TINY)
        assertFalse(store.installed())
        store.download()
        assertTrue(store.installed())
        assertEquals(DownloadState.Done, store.state.value)
        assertFalse(File(store.encoder().parentFile, "tiny-encoder.onnx").exists())
        assertEquals(SpeechModels.TINY.url, gw.requestedUrl) // the exact allowlisted URL
        assertEquals(SpeechModels.TINY.sha256, gw.requestedSha) // with its checksum
        assertEquals(1, gw.deleted) // the gateway copy is removed afterwards
    }

    @Test fun deniedOrFailedDownloadsGiveAPlainSentence() = runBlocking {
        val denied = ModelStore(ctx, FakeGateway(GatewayResult(false, Contract.DENIED, "", null, 0)), SpeechModels.BASE)
        denied.download()
        assertEquals(DownloadState.Failed(Errors.GW_NO), denied.state.value)
        val missing = ModelStore(ctx, FakeGateway(GatewayResult(false, "MISSING", "", null, 0)), SpeechModels.BASE)
        missing.download()
        assertEquals(DownloadState.Failed(Errors.GW_MISSING), missing.state.value)
        val broken = ModelStore(ctx, FakeGateway(ok, tarBz2(mapOf("tiny-tokens.txt" to "t"))), SpeechModels.TINY)
        broken.download()
        assertTrue(broken.state.value is DownloadState.Failed)
        assertFalse(broken.installed())
    }

    @Test fun userMessagesAreCodedSentences() {
        for (c in listOf(Contract.DENIED, Contract.EXPIRED, Contract.NOT_ALLOWED, Contract.WIFI_JOIN, Contract.WATCH_REFUSED, Contract.SIZE_MISMATCH, Contract.HASH_MISMATCH, "MISSING", "SECURITY", Contract.HTTP_ERROR)) {
            val m = GatewayResult(false, c, "", null, 0).userMessage()
            assertTrue("$c -> $m", Regex("^[A-Z]{2}-[0-9]{2}: .+[.]$").containsMatchIn(m))
        }
        assertEquals("", GatewayResult(true, Contract.OK, "", null, 1).userMessage())
    }

    @Test fun modelSpecsMatchThePolicy() {
        val p = com.nunna.embershared.GatewayPolicy
        for (u in listOf(SpeechModels.TINY.url, SpeechModels.BASE.url, LlmModelSpec.URL)) assertTrue(u, p.isAllowedModelUrl(u))
        assertEquals(64, LlmModelSpec.SHA256.length); assertEquals(64, SpeechModels.TINY.sha256.length)
    }
}
