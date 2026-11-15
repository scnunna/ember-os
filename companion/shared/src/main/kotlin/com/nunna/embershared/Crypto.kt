package com.nunna.embershared

import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * AES-256-GCM box: output = 12-byte random IV + ciphertext + 16-byte tag. The key comes from the caller; on Android
 * it is an AndroidKeyStore key that never leaves secure hardware/keystore, in tests it is an in-memory key.
 */
class AesGcmBox(private val key: () -> SecretKey) {

    fun encrypt(plain: ByteArray, aad: ByteArray = ByteArray(0)): ByteArray {
        // The cipher picks the random 12-byte IV itself. Android Keystore keys REQUIRE this ("Caller-provided IV not
        // permitted": randomized encryption is enforced), and the plain JCE does the same, so one code path fits both.
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, key())
        val iv = c.iv
        check(iv != null && iv.size == 12) { "unexpected GCM IV" }
        if (aad.isNotEmpty()) c.updateAAD(aad)
        return iv + c.doFinal(plain)
    }

    /** Throws if the data was changed or the key is wrong. */
    fun decrypt(box: ByteArray, aad: ByteArray = ByteArray(0)): ByteArray {
        require(box.size >= 12 + 16) { "too short" }
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, box.copyOfRange(0, 12)))
        if (aad.isNotEmpty()) c.updateAAD(aad)
        return c.doFinal(box, 12, box.size - 12)
    }

    fun encryptString(s: String, aad: String = "") = encrypt(s.toByteArray(Charsets.UTF_8), aad.toByteArray())
    fun decryptString(b: ByteArray, aad: String = "") = String(decrypt(b, aad.toByteArray()), Charsets.UTF_8)
}
