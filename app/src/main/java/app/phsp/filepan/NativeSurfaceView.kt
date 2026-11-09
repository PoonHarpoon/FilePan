// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.annotation.SuppressLint
import android.content.Context
import android.view.MotionEvent
import android.view.SurfaceView
import android.view.accessibility.AccessibilityNodeProvider

/**
 * The surface the native UI renders into. It forwards touches to native code, one JNI call per
 * MotionEvent with every pointer and historical sample, and exposes the native semantics to
 * accessibility services through [accessibility].
 */
@SuppressLint("ViewConstructor")
class NativeSurfaceView(context: Context, private val host: NativeHost) : SurfaceView(context) {
    var accessibility: A11yBridge? = null

    // Reused across events: per sample (pointer id, action, kind), (x, y) and the time in ns.
    private var ints = IntArray(3 * 16)
    private var coords = FloatArray(2 * 16)
    private var times = LongArray(16)
    private var count = 0

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun getAccessibilityNodeProvider(): AccessibilityNodeProvider? = accessibility

    override fun dispatchHoverEvent(event: MotionEvent): Boolean {
        // Explore by touch: TalkBack turns touches into hover events.
        if (accessibility?.onHover(event) == true) return true
        return super.dispatchHoverEvent(event)
    }

    @SuppressLint("ClickableViewAccessibility") // Clicks are native semantics actions.
    override fun onTouchEvent(event: MotionEvent): Boolean {
        count = 0
        val action = event.actionMasked
        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                add(event, event.actionIndex, ACTION_DOWN, -1)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                addHistory(event)
                add(event, event.actionIndex, ACTION_UP, -1)
            }
            MotionEvent.ACTION_MOVE -> {
                addHistory(event)
                for (i in 0 until event.pointerCount) add(event, i, ACTION_MOVE, -1)
            }
            MotionEvent.ACTION_CANCEL -> {
                for (i in 0 until event.pointerCount) add(event, i, ACTION_CANCEL, -1)
            }
            else -> return false
        }
        host.pointer(count, ints, coords, times)
        return true
    }

    private fun addHistory(event: MotionEvent) {
        for (h in 0 until event.historySize) {
            for (i in 0 until event.pointerCount) add(event, i, ACTION_MOVE, h)
        }
    }

    // history -1 is the current sample.
    private fun add(event: MotionEvent, index: Int, action: Int, history: Int) {
        if (count == times.size) {
            ints = ints.copyOf(ints.size * 2)
            coords = coords.copyOf(coords.size * 2)
            times = times.copyOf(times.size * 2)
        }
        ints[count * 3] = event.getPointerId(index)
        ints[count * 3 + 1] = action
        ints[count * 3 + 2] = when (event.getToolType(index)) {
            MotionEvent.TOOL_TYPE_MOUSE -> KIND_MOUSE
            MotionEvent.TOOL_TYPE_STYLUS, MotionEvent.TOOL_TYPE_ERASER -> KIND_STYLUS
            else -> KIND_TOUCH
        }
        if (history < 0) {
            coords[count * 2] = event.getX(index)
            coords[count * 2 + 1] = event.getY(index)
            times[count] = event.eventTimeNanos
        } else {
            coords[count * 2] = event.getHistoricalX(index, history)
            coords[count * 2 + 1] = event.getHistoricalY(index, history)
            times[count] = event.getHistoricalEventTimeNanos(history)
        }
        ++count
    }

    private companion object {
        // ui::PointerAction and ui::PointerKind.
        const val ACTION_DOWN = 0
        const val ACTION_MOVE = 1
        const val ACTION_UP = 2
        const val ACTION_CANCEL = 3
        const val KIND_TOUCH = 0
        const val KIND_MOUSE = 1
        const val KIND_STYLUS = 2
    }
}
