// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.io.FileNotFoundException
import java.util.concurrent.ConcurrentHashMap

/**
 * Serves files FilePan hands to other apps (open with, share) as
 * content://app.phsp.filepan.files/<path>, read-only, and the one another app asked FilePan to
 * create (ACTION_CREATE_DOCUMENT), which it may write. Only paths granted in this process are
 * served; the receiving app gets access through FLAG_GRANT_READ_URI_PERMISSION (and WRITE),
 * since the provider isn't exported.
 */
class FileContentProvider : ContentProvider() {
    override fun onCreate() = true

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?,
    ): Cursor {
        val file = fileFor(uri)
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        val cursor = MatrixCursor(columns, 1)
        val row = columns.map { column ->
            when (column) {
                OpenableColumns.DISPLAY_NAME -> file.name
                OpenableColumns.SIZE -> file.length()
                else -> null
            }
        }
        cursor.addRow(row)
        return cursor
    }

    override fun getType(uri: Uri): String? = uri.path?.let { grants[it]?.mimeType }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        val writable = uri.path?.let { grants[it] }?.writable == true
        if (mode != "r" && !writable) throw FileNotFoundException("Read-only: $uri")
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.parseMode(mode))
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0

    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<out String>?,
    ) = 0

    private fun fileFor(uri: Uri): File {
        val path = uri.path
        if (path == null || !grants.containsKey(path)) throw FileNotFoundException("Not shared: $uri")
        return File(path)
    }

    companion object {
        const val AUTHORITY = "app.phsp.filepan.files"

        private class Grant(val mimeType: String, val writable: Boolean)

        // Path -> what it was granted as, for the life of the process.
        private val grants = ConcurrentHashMap<String, Grant>()

        fun grant(path: String, mimeType: String, writable: Boolean = false): Uri {
            grants[path] = Grant(mimeType, writable)
            return Uri.Builder().scheme("content").authority(AUTHORITY).encodedPath(Uri.encode(path, "/")).build()
        }
    }
}
