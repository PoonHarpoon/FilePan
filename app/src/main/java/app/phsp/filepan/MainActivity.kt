// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.Manifest
import android.animation.ValueAnimator
import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.util.Log
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.accessibility.AccessibilityManager
import android.widget.FrameLayout
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher

/**
 * Hosts the native UI: a full-window [NativeSurfaceView] drawn edge to edge, and the [ImeBridge]
 * behind it. Everything that happens here is forwarded to [NativeHost]; there is no app logic.
 */
open class MainActivity : Activity() {
    protected lateinit var host: NativeHost
    lateinit var surfaceView: NativeSurfaceView
        private set
    lateinit var imeBridge: ImeBridge
        private set
    var accessibility: A11yBridge? = null
        private set

    private lateinit var accessibilityManager: AccessibilityManager
    private lateinit var powerManager: PowerManager
    private var insets: WindowInsets? = null

    private val backCallback = OnBackInvokedCallback { host.back() }

    private val powerSaveReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) = sendEnvironment()
    }
    private val durationScaleListener = ValueAnimator.DurationScaleChangeListener { sendEnvironment() }
    private val accessibilityListener = AccessibilityManager.AccessibilityStateChangeListener { updateAccessibility() }
    private val touchExplorationListener =
        AccessibilityManager.TouchExplorationStateChangeListener { updateAccessibility() }

    private val surfaceCallback = object : SurfaceHolder.Callback2 {
        override fun surfaceCreated(holder: SurfaceHolder) = host.surfaceCreated(holder.surface)

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) =
            host.surfaceChanged(width, height)

        // Blocks until the render thread has let go of the window.
        override fun surfaceDestroyed(holder: SurfaceHolder) = host.surfaceDestroyed()

        override fun surfaceRedrawNeeded(holder: SurfaceHolder) = host.redraw()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        host = NativeHost.get(this)
        accessibilityManager = getSystemService(AccessibilityManager::class.java)
        powerManager = getSystemService(PowerManager::class.java)

        // Edge to edge (enforced from targetSdk 35), including under cutouts.
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }

        imeBridge = ImeBridge(this, host)
        surfaceView = NativeSurfaceView(this, host)
        val root = FrameLayout(this)
        // Nothing in the window takes part in autofill: no fill-in suggestions, no saving prompts.
        root.importantForAutofill = ViewGroup.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
        // Behind the surface, so it never gets touches; it only needs focus for the IME.
        root.addView(imeBridge, FrameLayout.LayoutParams(1, 1))
        root.addView(
            surfaceView,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )
        setContentView(root)
        surfaceView.holder.addCallback(surfaceCallback)
        surfaceView.setOnApplyWindowInsetsListener { _, windowInsets ->
            insets = windowInsets
            sendEnvironment()
            WindowInsets.CONSUMED
        }
        surfaceView.requestFocus()

        onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback)
        registerReceiver(
            powerSaveReceiver,
            IntentFilter(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED),
            RECEIVER_NOT_EXPORTED,
        )
        ValueAnimator.registerDurationScaleChangeListener(durationScaleListener)
        accessibilityManager.addAccessibilityStateChangeListener(accessibilityListener)
        accessibilityManager.addTouchExplorationStateChangeListener(touchExplorationListener)

        host.activity = this
        sendEnvironment()
        updateAccessibility()
        // A process recreated with saved state already had this intent.
        if (savedInstanceState == null) receive(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        receive(intent)
    }

    /**
     * Hands what another app shared to [NativeHost], once: the intent is replaced afterwards, and
     * one restored from history or recents isn't taken again.
     */
    protected open fun receive(intent: Intent?) {
        if (intent == null || (intent.flags and Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0) return
        if (intent.action != Intent.ACTION_SEND && intent.action != Intent.ACTION_SEND_MULTIPLE) return
        // FilePan's own share sheet can offer FilePan; it then stays rather than leaving itself.
        host.receiveShare(intent, referrer?.host != packageName)
        setIntent(Intent(Intent.ACTION_MAIN))
    }

    override fun onDestroy() {
        if (host.activity === this) host.activity = null
        onBackInvokedDispatcher.unregisterOnBackInvokedCallback(backCallback)
        unregisterReceiver(powerSaveReceiver)
        ValueAnimator.unregisterDurationScaleChangeListener(durationScaleListener)
        accessibilityManager.removeAccessibilityStateChangeListener(accessibilityListener)
        accessibilityManager.removeTouchExplorationStateChangeListener(touchExplorationListener)
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        host.activity = this
        // Settings may have changed while away (animation scale, all-files access).
        sendEnvironment()
        host.resumed(true)
    }

    override fun onPause() {
        host.resumed(false)
        super.onPause()
    }

    // The manifest keeps the activity across configuration changes; density, font scale and
    // window size arrive here and through the surface and insets.
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        sendEnvironment()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Back goes through OnBackInvokedCallback; volume, power and the like stay the system's.
        if (event.keyCode == KeyEvent.KEYCODE_BACK || event.isSystem) return super.dispatchKeyEvent(event)
        val action = when (event.action) {
            KeyEvent.ACTION_DOWN -> 0
            KeyEvent.ACTION_UP -> 1
            else -> return super.dispatchKeyEvent(event)
        }
        host.key(
            event.keyCode, action, event.metaState, event.repeatCount,
            event.getUnicodeChar(event.metaState), event.eventTime * 1_000_000L, // uptimeMillis: CLOCK_MONOTONIC
        )
        return true
    }

    fun requestNotificationPermission() {
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
        }
    }

    /**
     * Opens the system document picker for one openable document; [NativeHost.documentResult] gets
     * the outcome. False if there's no picker to open.
     */
    /** Dark icons on the system bars for a light theme, light ones otherwise. */
    fun applySystemBars(light: Boolean) {
        val flags = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS or
            WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
        window.insetsController?.setSystemBarsAppearance(if (light) flags else 0, flags)
    }

    fun pickDocument(mimeTypes: Array<String>): Boolean {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
        if (mimeTypes.size > 1) {
            intent.type = "*/*"
            intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes)
        } else {
            intent.type = mimeTypes.firstOrNull() ?: "*/*"
        }
        return try {
            startActivityForResult(intent, REQUEST_PICK_DOCUMENT)
            true
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No document picker", e)
            false
        }
    }

    /**
     * Opens the system picker for where to save one document, suggesting [name];
     * [NativeHost.createDocumentResult] gets the outcome. False if there's no picker to open.
     */
    fun createDocument(name: String, mimeType: String): Boolean {
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType(mimeType)
            .putExtra(Intent.EXTRA_TITLE, name)
        return try {
            startActivityForResult(intent, REQUEST_CREATE_DOCUMENT)
            true
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No document picker", e)
            false
        }
    }

    /** Opens the system picker for a folder to keep; [NativeHost.documentTreeResult] gets it. */
    fun openDocumentTree(): Boolean = try {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_OPEN_DOCUMENT_TREE)
        true
    } catch (e: ActivityNotFoundException) {
        Log.w(TAG, "No folder picker", e)
        false
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQUEST_OPEN_DOCUMENT_TREE) {
            host.documentTreeResult(if (resultCode == RESULT_OK) data?.data else null)
            return
        }
        if (requestCode == REQUEST_PICK_DOCUMENT) {
            host.documentResult(if (resultCode == RESULT_OK) data?.data else null)
            return
        }
        if (requestCode == REQUEST_CREATE_DOCUMENT) {
            host.createDocumentResult(if (resultCode == RESULT_OK) data?.data else null)
            return
        }
        super.onActivityResult(requestCode, resultCode, data)
    }

    private fun updateAccessibility() {
        val enabled = accessibilityManager.isEnabled
        if (enabled && accessibility == null) {
            accessibility = A11yBridge(surfaceView, host)
            surfaceView.accessibility = accessibility
            host.requestFullSemantics()
        } else if (!enabled) {
            accessibility?.reset()
        }
        host.accessibilityEnabled(enabled)
    }

    private fun sendEnvironment() {
        val metrics = resources.displayMetrics
        val windowInsets = insets
        val safe = windowInsets?.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
        val ime = windowInsets?.getInsets(WindowInsets.Type.ime())
        val transitionScale = Settings.Global.getFloat(contentResolver, Settings.Global.TRANSITION_ANIMATION_SCALE, 1f)
        val reducedMotion = !ValueAnimator.areAnimatorsEnabled() || transitionScale == 0f
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
        host.environment(
            floatArrayOf(
                metrics.density,
                resources.configuration.fontScale,
                (safe?.left ?: 0).toFloat(), (safe?.top ?: 0).toFloat(),
                (safe?.right ?: 0).toFloat(), (safe?.bottom ?: 0).toFloat(),
                (ime?.left ?: 0).toFloat(), (ime?.top ?: 0).toFloat(),
                (ime?.right ?: 0).toFloat(), (ime?.bottom ?: 0).toFloat(),
                if (reducedMotion) 1f else 0f,
                if (powerManager.isPowerSaveMode) 1f else 0f,
                if (night) 1f else 0f,
            ),
        )
    }

    private companion object {
        const val TAG = "FilePan"
        const val REQUEST_NOTIFICATIONS = 1
        const val REQUEST_PICK_DOCUMENT = 2
        const val REQUEST_CREATE_DOCUMENT = 3
        const val REQUEST_OPEN_DOCUMENT_TREE = 4
    }
}
