// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.graphics.Rect
import android.os.Bundle
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityManager
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction
import android.view.accessibility.AccessibilityNodeProvider

/**
 * Exposes the native UI's semantics to accessibility services as virtual views of
 * [NativeSurfaceView]. Node ids are the native ones; bounds come in window px, which are the
 * view's own coordinates since the surface fills the window. Main thread only.
 */
class A11yBridge(private val view: View, private val host: NativeHost) : AccessibilityNodeProvider() {
    private val manager = view.context.getSystemService(AccessibilityManager::class.java)
    private val tree = SemanticsTree()
    private var accessibilityFocused = 0
    private var hovered = 0
    private val location = IntArray(2)

    fun apply(update: SemanticsUpdate) {
        val touched = tree.apply(update)
        if (accessibilityFocused != 0 && tree[accessibilityFocused] == null) accessibilityFocused = 0
        if (hovered != 0 && tree[hovered] == null) hovered = 0
        if (!manager.isEnabled) return
        sendEvent(HOST, AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED, AccessibilityEvent.CONTENT_CHANGE_TYPE_SUBTREE)
        for (id in touched) {
            val node = tree[id] ?: continue
            if (id == accessibilityFocused || node.hasFlag(SemanticsWire.FLAG_LIVE_REGION)) {
                sendEvent(id, AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED, AccessibilityEvent.CONTENT_CHANGE_TYPE_TEXT)
            }
        }
    }

    /** The provider's view of the tree is gone (accessibility turned off). */
    fun reset() {
        tree.clear()
        accessibilityFocused = 0
        hovered = 0
    }

    fun announce(text: String) {
        @Suppress("DEPRECATION") // The native side has no live region to put a one-off message in.
        view.announceForAccessibility(text)
    }

    /** Explore by touch. */
    fun onHover(event: MotionEvent): Boolean {
        if (!manager.isEnabled || !manager.isTouchExplorationEnabled) return false
        return when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                val id = tree.hitTest(event.x, event.y)
                setHovered(id)
                id != 0
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                setHovered(0)
                true
            }
            else -> false
        }
    }

    private fun setHovered(id: Int) {
        if (id == hovered) return
        val previous = hovered
        hovered = id
        if (id != 0) sendEvent(id, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER)
        if (previous != 0) sendEvent(previous, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT)
    }

    override fun createAccessibilityNodeInfo(virtualViewId: Int): AccessibilityNodeInfo? {
        if (virtualViewId == HOST) {
            val info = AccessibilityNodeInfo(view)
            view.onInitializeAccessibilityNodeInfo(info)
            if (tree.rootId != 0) info.addChild(view, tree.rootId)
            return info
        }
        val node = tree[virtualViewId] ?: return null
        val info = AccessibilityNodeInfo(view, virtualViewId)
        info.packageName = view.context.packageName
        info.className = className(node.role)
        if (node.parent != 0 && tree[node.parent] != null) {
            info.setParent(view, node.parent)
        } else {
            info.setParent(view)
        }
        for (child in node.children) info.addChild(view, child)

        val bounds = Rect(node.left, node.top, node.right, node.bottom)
        val visible = bounds.intersect(0, 0, view.width, view.height)
        info.isVisibleToUser = visible
        info.setBoundsInWindow(bounds)
        view.getLocationOnScreen(location)
        bounds.offset(location[0], location[1])
        info.setBoundsInScreen(bounds)

        when (node.role) {
            SemanticsWire.ROLE_TEXT_FIELD -> {
                info.text = node.value
                info.hintText = node.hint
                info.isEditable = true
                info.isPassword = node.hasFlag(SemanticsWire.FLAG_PASSWORD)
                info.isMultiLine = node.hasFlag(SemanticsWire.FLAG_MULTILINE)
                info.contentDescription = node.label.ifEmpty { null }
            }
            SemanticsWire.ROLE_TEXT, SemanticsWire.ROLE_HEADING -> {
                info.text = node.label
                if (node.value.isNotEmpty()) info.stateDescription = node.value
            }
            else -> {
                info.contentDescription = node.label.ifEmpty { null }
                if (node.value.isNotEmpty()) info.stateDescription = node.value
                if (node.hint.isNotEmpty()) info.hintText = node.hint
            }
        }
        info.isHeading = node.role == SemanticsWire.ROLE_HEADING
        if (node.hasFlag(SemanticsWire.FLAG_CHECKABLE)) {
            info.isCheckable = true
            info.setChecked(
                if (node.hasFlag(SemanticsWire.FLAG_CHECKED)) AccessibilityNodeInfo.CHECKED_STATE_TRUE
                else AccessibilityNodeInfo.CHECKED_STATE_FALSE,
            )
        }
        info.isSelected = node.hasFlag(SemanticsWire.FLAG_SELECTED)
        info.isEnabled = !node.hasFlag(SemanticsWire.FLAG_DISABLED)
        info.isFocusable = node.hasFlag(SemanticsWire.FLAG_FOCUSABLE)
        info.isFocused = node.hasFlag(SemanticsWire.FLAG_FOCUSED)
        if (node.hasFlag(SemanticsWire.FLAG_LIVE_REGION)) {
            info.liveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
        }
        info.isClickable = node.hasAction(SemanticsWire.ACTION_CLICK)
        info.isLongClickable = node.hasAction(SemanticsWire.ACTION_LONG_CLICK)
        info.isScrollable = node.hasAction(SemanticsWire.ACTION_SCROLL_FORWARD) ||
            node.hasAction(SemanticsWire.ACTION_SCROLL_BACKWARD)

        for ((bit, action) in ACTIONS) {
            if (node.hasAction(bit)) info.addAction(action)
        }
        if (node.hasFlag(SemanticsWire.FLAG_EXPANDABLE)) {
            info.addAction(
                if (node.hasFlag(SemanticsWire.FLAG_EXPANDED)) AccessibilityAction.ACTION_COLLAPSE
                else AccessibilityAction.ACTION_EXPAND,
            )
        }
        for (custom in node.customActions) {
            info.addAction(AccessibilityAction(CUSTOM_ACTION_BASE + custom.id, custom.label))
        }
        info.isAccessibilityFocused = accessibilityFocused == virtualViewId
        info.addAction(
            if (accessibilityFocused == virtualViewId) AccessibilityAction.ACTION_CLEAR_ACCESSIBILITY_FOCUS
            else AccessibilityAction.ACTION_ACCESSIBILITY_FOCUS,
        )
        return info
    }

    override fun findFocus(focus: Int): AccessibilityNodeInfo? {
        val id = when (focus) {
            AccessibilityNodeInfo.FOCUS_ACCESSIBILITY -> accessibilityFocused
            AccessibilityNodeInfo.FOCUS_INPUT -> inputFocused()
            else -> 0
        }
        return if (id != 0) createAccessibilityNodeInfo(id) else null
    }

    override fun performAction(virtualViewId: Int, action: Int, arguments: Bundle?): Boolean {
        if (virtualViewId == HOST) return view.performAccessibilityAction(action, arguments)
        val node = tree[virtualViewId] ?: return false
        when (action) {
            AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS -> {
                if (accessibilityFocused == virtualViewId) return false
                val previous = accessibilityFocused
                accessibilityFocused = virtualViewId
                if (previous != 0) sendEvent(previous, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED)
                sendEvent(virtualViewId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED)
                return true
            }
            AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS -> {
                if (accessibilityFocused != virtualViewId) return false
                accessibilityFocused = 0
                sendEvent(virtualViewId, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED)
                return true
            }
        }
        if (action >= CUSTOM_ACTION_BASE) {
            val customId = action - CUSTOM_ACTION_BASE
            if (node.customActions.none { it.id == customId }) return false
            host.accessibilityAction(virtualViewId, SemanticsWire.ACTION_CUSTOM, customId, null, -1, -1)
            return true
        }
        val bit = ACTIONS.firstOrNull { it.second.id == action }?.first ?: when (action) {
            AccessibilityNodeInfo.ACTION_EXPAND -> SemanticsWire.ACTION_EXPAND
            AccessibilityNodeInfo.ACTION_COLLAPSE -> SemanticsWire.ACTION_COLLAPSE
            else -> return false
        }
        val expandable = bit == SemanticsWire.ACTION_EXPAND || bit == SemanticsWire.ACTION_COLLAPSE
        if (!node.hasAction(bit) && !(expandable && node.hasFlag(SemanticsWire.FLAG_EXPANDABLE))) return false
        var text: ByteArray? = null
        var selectionStart = -1
        var selectionEnd = -1
        when (bit) {
            SemanticsWire.ACTION_SET_TEXT -> {
                val value = arguments?.getCharSequence(
                    AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,
                ) ?: ""
                text = Utf8.encode(value)
            }
            SemanticsWire.ACTION_SET_SELECTION -> {
                // UTF-16 indices into the node's text, as native byte offsets.
                val start = arguments?.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, -1) ?: -1
                val end = arguments?.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, -1) ?: -1
                selectionStart = Utf8.utf16ToUtf8Offset(node.value, start)
                selectionEnd = Utf8.utf16ToUtf8Offset(node.value, end)
            }
        }
        host.accessibilityAction(virtualViewId, bit, 0, text, selectionStart, selectionEnd)
        if (bit == SemanticsWire.ACTION_CLICK) sendEvent(virtualViewId, AccessibilityEvent.TYPE_VIEW_CLICKED)
        if (bit == SemanticsWire.ACTION_LONG_CLICK) sendEvent(virtualViewId, AccessibilityEvent.TYPE_VIEW_LONG_CLICKED)
        return true
    }

    private fun inputFocused(): Int {
        val stack = ArrayDeque<Int>()
        if (tree.rootId != 0) stack.addLast(tree.rootId)
        while (stack.isNotEmpty()) {
            val node = tree[stack.removeLast()] ?: continue
            if (node.hasFlag(SemanticsWire.FLAG_FOCUSED)) return node.id
            for (child in node.children) stack.addLast(child)
        }
        return 0
    }

    private fun sendEvent(virtualViewId: Int, type: Int, contentChangeTypes: Int = 0) {
        if (!manager.isEnabled) return
        val parent = view.parent ?: return
        val event = AccessibilityEvent(type)
        event.packageName = view.context.packageName
        if (virtualViewId == HOST) {
            event.setSource(view)
            event.className = view.javaClass.name
        } else {
            val node = tree[virtualViewId]
            event.setSource(view, virtualViewId)
            event.className = node?.let { className(it.role) } ?: VIEW
            if (node != null) {
                val description = node.label.ifEmpty { node.value }
                if (description.isNotEmpty()) event.text.add(description)
            }
        }
        if (contentChangeTypes != 0) event.contentChangeTypes = contentChangeTypes
        parent.requestSendAccessibilityEvent(view, event)
    }

    private companion object {
        const val HOST = HOST_VIEW_ID
        // Custom action ids: above every standard action id, below the resource id range.
        const val CUSTOM_ACTION_BASE = 0x40000000
        const val VIEW = "android.view.View"

        val ACTIONS = listOf(
            SemanticsWire.ACTION_CLICK to AccessibilityAction.ACTION_CLICK,
            SemanticsWire.ACTION_LONG_CLICK to AccessibilityAction.ACTION_LONG_CLICK,
            SemanticsWire.ACTION_SCROLL_FORWARD to AccessibilityAction.ACTION_SCROLL_FORWARD,
            SemanticsWire.ACTION_SCROLL_BACKWARD to AccessibilityAction.ACTION_SCROLL_BACKWARD,
            SemanticsWire.ACTION_SET_TEXT to AccessibilityAction.ACTION_SET_TEXT,
            SemanticsWire.ACTION_FOCUS to AccessibilityAction.ACTION_FOCUS,
            SemanticsWire.ACTION_CLEAR_FOCUS to AccessibilityAction.ACTION_CLEAR_FOCUS,
            SemanticsWire.ACTION_DISMISS to AccessibilityAction.ACTION_DISMISS,
            SemanticsWire.ACTION_SET_SELECTION to AccessibilityAction.ACTION_SET_SELECTION,
        )

        fun className(role: Int): String = when (role) {
            SemanticsWire.ROLE_BUTTON, SemanticsWire.ROLE_TAB, SemanticsWire.ROLE_MENU_ITEM -> "android.widget.Button"
            SemanticsWire.ROLE_SWITCH -> "android.widget.Switch"
            SemanticsWire.ROLE_CHECKBOX -> "android.widget.CheckBox"
            SemanticsWire.ROLE_RADIO_BUTTON -> "android.widget.RadioButton"
            SemanticsWire.ROLE_TEXT, SemanticsWire.ROLE_HEADING -> "android.widget.TextView"
            SemanticsWire.ROLE_LIST, SemanticsWire.ROLE_MENU -> "android.widget.ListView"
            SemanticsWire.ROLE_GRID -> "android.widget.GridView"
            SemanticsWire.ROLE_TEXT_FIELD -> "android.widget.EditText"
            SemanticsWire.ROLE_IMAGE -> "android.widget.ImageView"
            SemanticsWire.ROLE_SCROLL -> "android.widget.ScrollView"
            SemanticsWire.ROLE_TAB_LIST -> "android.widget.TabWidget"
            SemanticsWire.ROLE_PROGRESS -> "android.widget.ProgressBar"
            SemanticsWire.ROLE_SLIDER -> "android.widget.SeekBar"
            else -> VIEW
        }
    }
}
