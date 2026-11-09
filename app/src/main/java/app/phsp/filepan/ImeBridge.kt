// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.annotation.SuppressLint
import android.content.Context
import android.text.Editable
import android.text.InputType
import android.text.Selection
import android.text.Spanned
import android.view.View
import android.view.WindowInsets
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

/**
 * The view the IME talks to while a native text field has focus. It keeps an Editable mirroring
 * the field: IME edits apply to it through BaseInputConnection, and after each edit (or batch) its
 * whole state goes to native code as UTF-8 with byte offsets. Changes from native code replace it.
 */
@SuppressLint("ViewConstructor")
class ImeBridge(context: Context, private val host: NativeHost) : View(context) {
    private val imm = context.getSystemService(InputMethodManager::class.java)
    private val editable: Editable = Editable.Factory.getInstance().newEditable("")
    private var config = ImeConfig(0, 6, 0)
    private var active = false
    // Native code's session for the focused field. Each InputConnection keeps the one it was made
    // for: an IME often sends a last edit (finishComposingText, a commit) to the old connection
    // after focus moved, and that must neither touch the new field's text nor reach native code
    // as an edit of the new field.
    private var session = 0L

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        // Never offered to the autofill service: no fill-in suggestions over the fields.
        importantForAutofill = IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
    }

    override fun onCheckIsTextEditor() = active

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        if (!active) return null
        outAttrs.inputType = inputType(config)
        outAttrs.imeOptions = config.action or EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
        outAttrs.initialSelStart = Selection.getSelectionStart(editable)
        outAttrs.initialSelEnd = Selection.getSelectionEnd(editable)
        outAttrs.setInitialSurroundingText(editable)
        return Connection(session)
    }

    /** Native code focused a text field. */
    fun show(state: ImeState, config: ImeConfig, session: Long, caretLeft: Int, caretTop: Int, caretRight: Int, caretBottom: Int) {
        this.config = config
        this.session = session
        active = true
        set(state)
        requestFocus()
        imm.restartInput(this)
        windowInsetsController?.show(WindowInsets.Type.ime())
        // The caret, so accessibility and the IME know where the field is.
        requestRectangleOnScreen(android.graphics.Rect(caretLeft, caretTop, caretRight, caretBottom))
    }

    // A request to show the keyboard while another window has focus (e.g. the USB permission
    // dialog) is dropped; ask again once the window has it back.
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        if (hasWindowFocus && active) {
            requestFocus()
            imm.restartInput(this)
            windowInsetsController?.show(WindowInsets.Type.ime())
        }
    }

    fun hide() {
        if (!active) return
        active = false
        windowInsetsController?.hide(WindowInsets.Type.ime())
        imm.restartInput(this)
        clearFocus()
        editable.clear()
    }

    /** The field's value changed from the app side. */
    fun update(state: ImeState, session: Long) {
        if (!active || session != this.session) return
        val textChanged = !Utf8.encode(editable).contentEquals(state.text)
        set(state)
        if (textChanged) {
            imm.restartInput(this)
        } else {
            notifySelection()
        }
    }

    private fun set(state: ImeState) {
        val text = Utf8.decode(state.text)
        editable.replace(0, editable.length, text)
        val start = Utf8.utf8ToUtf16Offset(state.text, state.selectionStart).coerceIn(0, editable.length)
        val end = Utf8.utf8ToUtf16Offset(state.text, state.selectionEnd).coerceIn(0, editable.length)
        Selection.setSelection(editable, start, end)
        BaseInputConnection.removeComposingSpans(editable)
        if (state.composingStart >= 0 && state.composingEnd > state.composingStart) {
            // What BaseInputConnection.setComposingRegion marks the region with.
            editable.setSpan(
                ComposingSpan(),
                Utf8.utf8ToUtf16Offset(state.text, state.composingStart).coerceIn(0, editable.length),
                Utf8.utf8ToUtf16Offset(state.text, state.composingEnd).coerceIn(0, editable.length),
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE or Spanned.SPAN_COMPOSING,
            )
        }
    }

    private fun notifySelection() {
        imm.updateSelection(
            this,
            Selection.getSelectionStart(editable),
            Selection.getSelectionEnd(editable),
            BaseInputConnection.getComposingSpanStart(editable),
            BaseInputConnection.getComposingSpanEnd(editable),
        )
    }

    // The IME edited the text: send the whole state to native code.
    private fun push(session: Long) {
        val text = editable.toString()
        val bytes = Utf8.encode(text)
        host.imeValue(
            bytes,
            Utf8.utf16ToUtf8Offset(text, Selection.getSelectionStart(editable)),
            Utf8.utf16ToUtf8Offset(text, Selection.getSelectionEnd(editable)),
            Utf8.utf16ToUtf8Offset(text, BaseInputConnection.getComposingSpanStart(editable)),
            Utf8.utf16ToUtf8Offset(text, BaseInputConnection.getComposingSpanEnd(editable)),
            session,
        )
        notifySelection()
    }

    private inner class Connection(private val session: Long) : BaseInputConnection(this@ImeBridge, true) {
        private var batch = 0

        // Still the focused field's connection; a stale one does nothing.
        private val current get() = active && session == this@ImeBridge.session

        override fun getEditable(): Editable = this@ImeBridge.editable

        override fun beginBatchEdit(): Boolean {
            ++batch
            return true
        }

        override fun endBatchEdit(): Boolean {
            if (batch > 0 && --batch == 0 && current) push(session)
            return batch > 0
        }

        private fun changed(result: Boolean): Boolean {
            if (batch == 0) push(session)
            return result
        }

        private inline fun edit(apply: () -> Boolean) = if (current) changed(apply()) else false

        override fun commitText(text: CharSequence?, newCursorPosition: Int) =
            edit { super.commitText(text, newCursorPosition) }

        override fun setComposingText(text: CharSequence?, newCursorPosition: Int) =
            edit { super.setComposingText(text, newCursorPosition) }

        override fun setComposingRegion(start: Int, end: Int) = edit { super.setComposingRegion(start, end) }

        override fun finishComposingText() = edit { super.finishComposingText() }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int) =
            edit { super.deleteSurroundingText(beforeLength, afterLength) }

        override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int) =
            edit { super.deleteSurroundingTextInCodePoints(beforeLength, afterLength) }

        override fun setSelection(start: Int, end: Int) = edit { super.setSelection(start, end) }

        override fun performEditorAction(editorAction: Int): Boolean {
            if (!current) return false
            host.imeAction(editorAction, session)
            return true
        }

        // sendKeyEvent (Enter, Del from some IMEs) keeps BaseInputConnection's behaviour: the key
        // goes through the view hierarchy to MainActivity.dispatchKeyEvent, then native code.
    }

    private class ComposingSpan

    private companion object {
        fun inputType(config: ImeConfig): Int {
            val multiline = config.flags and 1 != 0
            var type = when (config.keyboard) {
                1 -> InputType.TYPE_CLASS_NUMBER
                2 -> InputType.TYPE_CLASS_PHONE
                3 -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS
                4 -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
                5 -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                6 -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                7 -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
                else -> InputType.TYPE_CLASS_TEXT
            }
            if (type and InputType.TYPE_MASK_CLASS == InputType.TYPE_CLASS_TEXT) {
                if (multiline) type = type or InputType.TYPE_TEXT_FLAG_MULTI_LINE
                // No suggestion strip, predictions or autocorrection, in any field.
                type = type or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            }
            return type
        }
    }
}
