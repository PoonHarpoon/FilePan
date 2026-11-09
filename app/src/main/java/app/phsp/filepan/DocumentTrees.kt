// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.content.ContentResolver
import android.database.Cursor
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.webkit.MimeTypeMap
import java.io.ByteArrayOutputStream
import java.io.FileNotFoundException
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The document calls behind document trees: folders the user granted with ACTION_OPEN_DOCUMENT_TREE,
 * kept with a persistable permission. jni/document_provider.cpp calls these on native I/O threads
 * (ContentResolver is thread-safe), so they may block. A tree is its URI as granted; documents are
 * the provider's ids.
 *
 * Failures are thrown and mapped to errnos natively: FileNotFoundException to ENOENT,
 * SecurityException (a revoked grant) to EACCES, UnsupportedOperationException to ENOTSUP, anything
 * else to EIO.
 *
 * Documents cross as little-endian records: str id, str name, u8 directory, u64 size, i64 modified
 * ms, u32 flags (DocumentInfo's), where a str is a u32 length and UTF-8.
 */
class DocumentTrees(private val resolver: ContentResolver) {
    fun root(tree: String): ByteArray {
        val treeUri = Uri.parse(tree)
        val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, DocumentsContract.getTreeDocumentId(treeUri))
        return query(uri).use { cursor ->
            if (!cursor.moveToFirst()) throw FileNotFoundException(tree)
            record(cursor).toByteArray()
        }
    }

    /** The tree's top folder's display name, or null if the provider gives none. */
    fun displayName(tree: String): String? = try {
        val treeUri = Uri.parse(tree)
        val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, DocumentsContract.getTreeDocumentId(treeUri))
        query(uri).use { cursor -> if (cursor.moveToFirst()) cursor.getString(1) else null }
    } catch (e: Exception) {
        null
    }

    fun children(tree: String, documentId: String): ByteArray {
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(Uri.parse(tree), documentId)
        val out = ByteArrayOutputStream()
        query(uri).use { cursor ->
            out.write(le(4).putInt(cursor.count).array())
            while (cursor.moveToNext()) {
                record(cursor, out)
            }
        }
        return out.toByteArray()
    }

    fun create(tree: String, parentId: String, name: String, directory: Boolean): ByteArray {
        val mimeType = if (directory) {
            Document.MIME_TYPE_DIR
        } else {
            MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase())
                ?: "application/octet-stream"
        }
        val created = DocumentsContract.createDocument(resolver, documentUri(tree, parentId), mimeType, name)
            ?: throw IOException("Couldn't create $name")
        return query(created).use { cursor ->
            if (!cursor.moveToFirst()) throw FileNotFoundException(name)
            record(cursor).toByteArray()
        }
    }

    fun rename(tree: String, documentId: String, name: String): String {
        val renamed = DocumentsContract.renameDocument(resolver, documentUri(tree, documentId), name)
        // Null when the id stays the same.
        return renamed?.let { DocumentsContract.getDocumentId(it) } ?: documentId
    }

    fun move(tree: String, documentId: String, fromParentId: String, toParentId: String): String {
        val uri = documentUri(tree, documentId)
        val flags = query(uri).use { cursor ->
            if (!cursor.moveToFirst()) throw FileNotFoundException(documentId)
            cursor.getInt(cursor.getColumnIndexOrThrow(Document.COLUMN_FLAGS))
        }
        if ((flags and Document.FLAG_SUPPORTS_MOVE) == 0) throw UnsupportedOperationException("No move")
        val moved = DocumentsContract.moveDocument(
            resolver, uri, documentUri(tree, fromParentId), documentUri(tree, toParentId),
        ) ?: throw IOException("Couldn't move $documentId")
        return DocumentsContract.getDocumentId(moved)
    }

    fun remove(tree: String, documentId: String) {
        if (!DocumentsContract.deleteDocument(resolver, documentUri(tree, documentId))) {
            throw IOException("Couldn't delete $documentId")
        }
    }

    /** A file descriptor native code then owns; mode is "r", "w", "wt" or "rw". */
    fun open(tree: String, documentId: String, mode: String): Int {
        val descriptor = resolver.openFileDescriptor(documentUri(tree, documentId), mode)
            ?: throw FileNotFoundException(documentId)
        return descriptor.detachFd()
    }

    private fun documentUri(tree: String, documentId: String): Uri =
        DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree), documentId)

    private fun query(uri: Uri): Cursor =
        resolver.query(uri, COLUMNS, null, null, null) ?: throw FileNotFoundException(uri.toString())

    private fun record(cursor: Cursor, out: ByteArrayOutputStream = ByteArrayOutputStream()): ByteArrayOutputStream {
        val mimeType = cursor.getString(2)
        val directory = mimeType == Document.MIME_TYPE_DIR
        val documentFlags = cursor.getInt(5)
        var flags = 0
        val writable = if (directory) Document.FLAG_DIR_SUPPORTS_CREATE else Document.FLAG_SUPPORTS_WRITE
        if ((documentFlags and writable) != 0) {
            flags = flags or WRITE
        }
        if ((documentFlags and Document.FLAG_SUPPORTS_DELETE) != 0) flags = flags or DELETE
        if ((documentFlags and Document.FLAG_SUPPORTS_RENAME) != 0) flags = flags or RENAME
        if ((documentFlags and Document.FLAG_SUPPORTS_MOVE) != 0) flags = flags or MOVE
        writeString(out, cursor.getString(0) ?: "")
        writeString(out, cursor.getString(1) ?: "")
        out.write(if (directory) 1 else 0)
        out.write(
            le(20)
                .putLong(if (cursor.isNull(3)) 0 else cursor.getLong(3))
                .putLong(if (cursor.isNull(4)) 0 else cursor.getLong(4))
                .putInt(flags)
                .array(),
        )
        return out
    }

    private fun writeString(out: ByteArrayOutputStream, value: String) {
        val bytes = Utf8.encode(value)
        out.write(le(4).putInt(bytes.size).array())
        out.write(bytes)
    }

    private fun le(size: Int): ByteBuffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)

    private companion object {
        val COLUMNS = arrayOf(
            Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_MIME_TYPE,
            Document.COLUMN_SIZE,
            Document.COLUMN_LAST_MODIFIED,
            Document.COLUMN_FLAGS,
        )

        // DocumentInfo's flags.
        const val WRITE = 1
        const val DELETE = 2
        const val RENAME = 4
        const val MOVE = 8
    }
}
