package com.nunna.embergateway

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import com.nunna.embershared.AesGcmBox
import com.nunna.embershared.AuditEntry
import com.nunna.embershared.AuditLog
import com.nunna.embershared.GatewayRequest
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withTimeoutOrNull
import java.io.File
import java.security.KeyStore
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

data class PendingApproval(val id: String, val request: GatewayRequest, val reason: String, val createdMs: Long)

/** Pending approvals + the encrypted audit log. One per process. */
object GatewayStore {
    private lateinit var ctx: Context
    private lateinit var box: AesGcmBox
    private lateinit var auditFile: File
    private val _pending = MutableStateFlow<List<PendingApproval>>(emptyList())
    val pending: StateFlow<List<PendingApproval>> = _pending
    private val _audit = MutableStateFlow<List<AuditEntry>>(emptyList())
    val audit: StateFlow<List<AuditEntry>> = _audit
    private val waiting = HashMap<String, CompletableDeferred<Boolean>>()
    private var ready = false

    const val APPROVAL_TIMEOUT_MS = 5 * 60_000L

    @Synchronized
    fun init(context: Context) {
        if (ready) return
        ctx = context.applicationContext
        box = AesGcmBox { keystoreKey() }
        auditFile = File(ctx.filesDir, "audit.bin")
        _audit.value = runCatching { AuditLog.decode(box.decryptString(auditFile.readBytes(), "audit")) }.getOrDefault(emptyList())
        File(ctx.filesDir, "out").apply { mkdirs() }.listFiles()?.forEach { if (System.currentTimeMillis() - it.lastModified() > 3_600_000) it.delete() }
        ready = true
    }

    private fun keystoreKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey("gateway_audit", null) as? SecretKey)?.let { return it }
        val g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        g.init(
            KeyGenParameterSpec.Builder("gateway_audit", KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build(),
        )
        return g.generateKey()
    }

    fun outDir() = File(ctx.filesDir, "out").apply { mkdirs() }

    @Synchronized
    fun log(e: AuditEntry) {
        _audit.value = AuditLog.add(_audit.value, e.copy(url = AuditLog.scrub(e.url)))
        runCatching { auditFile.writeBytes(box.encryptString(AuditLog.encode(_audit.value), "audit")) }
    }

    /** Puts a request on the approval screen and waits for the user (5 minutes, then it expires). */
    suspend fun awaitApproval(p: PendingApproval): Boolean? {
        val d = CompletableDeferred<Boolean>()
        synchronized(this) { waiting[p.id] = d }
        _pending.update { it + p }
        val r = withTimeoutOrNull(APPROVAL_TIMEOUT_MS) { d.await() }
        synchronized(this) { waiting.remove(p.id) }
        _pending.update { l -> l.filter { it.id != p.id } }
        return r
    }

    fun decide(id: String, approve: Boolean) {
        synchronized(this) { waiting[id] }?.complete(approve)
    }
}
