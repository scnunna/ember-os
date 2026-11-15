package com.nunna.embergateway

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File
import java.io.FileNotFoundException

/**
 * Hands finished downloads to Ember Companion as content://com.nunna.embergateway.files/out/<id>.
 * The manifest protects the whole provider with the signature permission. Files are read-only and can be deleted
 * by the client once it has copied them.
 */
class GatewayProvider : ContentProvider() {
    override fun onCreate(): Boolean = true

    private fun fileFor(uri: Uri): File {
        val segs = uri.pathSegments
        if (segs.size != 2 || segs[0] != "out" || !Regex("^[0-9a-f-]{36}$").matches(segs[1])) throw FileNotFoundException("bad uri")
        GatewayStore.init(context!!)
        return File(GatewayStore.outDir(), segs[1])
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw SecurityException("read only")
        val f = fileFor(uri)
        if (!f.exists()) throw FileNotFoundException()
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int =
        runCatching { if (fileFor(uri).delete()) 1 else 0 }.getOrDefault(0)

    override fun query(uri: Uri, p: Array<out String>?, s: String?, a: Array<out String>?, o: String?): Cursor? = null
    override fun getType(uri: Uri): String = "application/octet-stream"
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun update(uri: Uri, values: ContentValues?, s: String?, a: Array<out String>?): Int = 0
}
