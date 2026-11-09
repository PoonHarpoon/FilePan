// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.content.Intent
import android.os.Bundle

/**
 * Another app asking FilePan for a document: a new one to write (ACTION_CREATE_DOCUMENT), or files
 * to read (ACTION_GET_CONTENT, ACTION_OPEN_DOCUMENT). [MainActivity] is singleTask, so it can't
 * return a result to an app in another task; this one opens in the caller's task instead. It
 * shows the same native UI, which answers through [NativeHost.answerDocumentRequest] and then
 * finishes it.
 */
class DocumentRequestActivity : MainActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Brought back after the process died: the request it held went with it.
        if (savedInstanceState != null) finish()
    }

    override fun receive(intent: Intent?) {
        if (intent == null || intent.action !in ACTIONS ||
            (intent.flags and Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0
        ) {
            finish()
            return
        }
        host.receiveDocumentRequest(this, intent)
    }

    override fun onDestroy() {
        // Left without an answer (back, or swiped away): the native UI drops the request.
        host.documentRequestEnded(this)
        super.onDestroy()
    }

    private companion object {
        val ACTIONS = setOf(Intent.ACTION_CREATE_DOCUMENT, Intent.ACTION_GET_CONTENT, Intent.ACTION_OPEN_DOCUMENT)
    }
}
