package com.nunna.embercompanion.android

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.compressors.bzip2.BZip2CompressorInputStream
import java.io.File
import java.util.Locale
import java.util.UUID

/** A speech model the user may download (through Ember Gateway). The checksums were computed from the official release files. */
data class SpeechModelSpec(
    val id: String, val name: String, val url: String, val sizeBytes: Long, val sha256: String,
    val prefix: String, val diskMb: Int,
) {
    val downloadMb get() = (sizeBytes / 1_048_576).toInt()
}

object SpeechModels {
    val TINY = SpeechModelSpec(
        "tiny", "Whisper tiny (many languages)",
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-whisper-tiny.tar.bz2",
        116_204_861, "c46116994e539aa165266d96b325252728429c12535eb9d8b6a2b10f129e66b1", "tiny", 104,
    )
    val BASE = SpeechModelSpec(
        "base", "Whisper base (many languages, better, slower)",
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-whisper-base.tar.bz2",
        207_557_382, "911b2083efd7c0dca2ac3b358b75222660dc09fb716d64fbfc417ba6c99ff3de", "base", 190,
    )
    val all = listOf(TINY, BASE)
    fun byId(id: String) = all.firstOrNull { it.id == id } ?: TINY
}

sealed interface DownloadState {
    data object Idle : DownloadState
    /** Waiting for you to tap Approve in Ember Gateway. */
    data object WaitingApproval : DownloadState
    data class Running(val percent: Int) : DownloadState
    data object Unpacking : DownloadState
    data class Failed(val message: String) : DownloadState
    data object Done : DownloadState
}

/** Downloads a speech model through the Gateway (checksum verified there) and unpacks only the int8 files and tokens. */
class ModelStore(ctx: Context, private val gateway: GatewayApi, val spec: SpeechModelSpec) {
    private val root = File(ctx.filesDir, "models/whisper-${spec.id}")
    private val _state = MutableStateFlow<DownloadState>(if (installed()) DownloadState.Done else DownloadState.Idle)
    val state: StateFlow<DownloadState> = _state

    fun installed() = encoder().exists() && decoder().exists() && tokens().exists()
    fun encoder() = File(root, "${spec.prefix}-encoder.int8.onnx")
    fun decoder() = File(root, "${spec.prefix}-decoder.int8.onnx")
    fun tokens() = File(root, "${spec.prefix}-tokens.txt")

    fun delete() { root.deleteRecursively(); _state.value = DownloadState.Idle }

    suspend fun download() {
        if (_state.value is DownloadState.Running || _state.value is DownloadState.WaitingApproval) return
        _state.value = DownloadState.WaitingApproval
        val r = gateway.downloadModel(spec.url, spec.sizeBytes, spec.sha256, spec.name,
            onProgress = { _state.value = DownloadState.Running(it) }, onNeedsApproval = { _state.value = DownloadState.WaitingApproval })
        if (!r.ok || r.uri == null) { _state.value = DownloadState.Failed(r.userMessage()); return }
        val tmp = File(root.parentFile, "tmp-" + UUID.randomUUID())
        try {
            _state.value = DownloadState.Unpacking
            withContext(Dispatchers.IO) {
                tmp.mkdirs()
                val input = gateway.open(r.uri) ?: throw IllegalStateException("open failed")
                TarArchiveInputStream(BZip2CompressorInputStream(input.buffered(1 shl 16))).use { tar ->
                    while (true) {
                        val e = tar.nextEntry ?: break
                        if (e.isDirectory) continue
                        val name = e.name.substringAfterLast('/')
                        if (!(name.contains("int8") || name.endsWith("tokens.txt"))) continue
                        File(tmp, name).outputStream().use { o -> tar.copyTo(o) }
                    }
                }
                val ok = File(tmp, "${spec.prefix}-encoder.int8.onnx").exists() && File(tmp, "${spec.prefix}-decoder.int8.onnx").exists() && File(tmp, "${spec.prefix}-tokens.txt").exists()
                if (!ok) throw IllegalStateException("model files missing in archive")
                root.deleteRecursively()
                root.parentFile?.mkdirs()
                if (!tmp.renameTo(root)) throw IllegalStateException("could not move model into place")
            }
            _state.value = DownloadState.Done
        } catch (e: Exception) {
            tmp.deleteRecursively()
            _state.value = DownloadState.Failed("ST-03: The speech model could not be unpacked. Try the download again.")
        } finally {
            gateway.delete(r.uri)
        }
    }
}

/** The on-device language model used for translation and summaries (Gemma 4 E2B, LiteRT-LM format, Apache-2.0, not gated). */
object LlmModelSpec {
    const val NAME = "Gemma 4 E2B (on-device AI)"
    const val FILE = "gemma-4-E2B-it.litertlm"
    const val URL = "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it.litertlm"
    const val SIZE_BYTES = 2_588_147_712L
    const val SHA256 = "181938105e0eefd105961417e8da75903eacda102c4fce9ce90f50b97139a63c"
    const val LICENSE = "Apache-2.0 (public on Hugging Face, no sign-in needed)"
}

class LlmModelStore(ctx: Context, private val gateway: GatewayApi, private val prefs: () -> Prefs) {
    private val dir = File(ctx.filesDir, "models/gemma")
    private val _state = MutableStateFlow<DownloadState>(if (path() != null) DownloadState.Done else DownloadState.Idle)
    val state: StateFlow<DownloadState> = _state

    /** The model file to use: the downloaded one, or one the user imported by hand. */
    fun path(): String? {
        prefs().llmModelPath?.takeIf { File(it).exists() }?.let { return it }
        return File(dir, LlmModelSpec.FILE).takeIf { it.exists() && it.length() == LlmModelSpec.SIZE_BYTES }?.absolutePath
    }

    fun delete() { dir.deleteRecursively(); prefs().llmModelPath = null; _state.value = DownloadState.Idle }

    suspend fun download() {
        if (_state.value is DownloadState.Running || _state.value is DownloadState.WaitingApproval) return
        _state.value = DownloadState.WaitingApproval
        val r = gateway.downloadModel(LlmModelSpec.URL, LlmModelSpec.SIZE_BYTES, LlmModelSpec.SHA256, LlmModelSpec.NAME,
            onProgress = { _state.value = DownloadState.Running(it) }, onNeedsApproval = { _state.value = DownloadState.WaitingApproval })
        if (!r.ok || r.uri == null) { _state.value = DownloadState.Failed(r.userMessage()); return }
        try {
            _state.value = DownloadState.Unpacking
            withContext(Dispatchers.IO) {
                dir.mkdirs()
                val tmp = File(dir, "part")
                val input = gateway.open(r.uri) ?: throw IllegalStateException("open failed")
                input.use { i -> tmp.outputStream().use { o -> i.copyTo(o, 1 shl 20) } }
                if (tmp.length() != LlmModelSpec.SIZE_BYTES) { tmp.delete(); throw IllegalStateException("size") }
                val dest = File(dir, LlmModelSpec.FILE)
                dest.delete()
                if (!tmp.renameTo(dest)) throw IllegalStateException("move")
            }
            _state.value = DownloadState.Done
        } catch (e: Exception) {
            _state.value = DownloadState.Failed("LM-06: The AI model could not be saved. Make sure there are 3 GB free, then try again.")
        } finally {
            gateway.delete(r.uri)
        }
    }
}

/** Languages offered for translation and speech (the AI model and Whisper handle all of these). */
object Languages {
    val codes = listOf(
        "en", "fr", "es", "de", "it", "pt", "nl", "sv", "pl", "ru", "uk", "tr", "ar", "he", "fa", "hi", "bn", "ur", "ta", "te",
        "zh", "ja", "ko", "vi", "th", "id", "el", "cs", "ro", "hu",
    ).sortedBy { name(it) }

    fun name(code: String): String =
        Locale.forLanguageTag(code).getDisplayLanguage(Locale.ENGLISH).replaceFirstChar { it.uppercase() }.ifBlank { code }
}
