// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.Manifest
import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.Uri
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.StatFs
import android.os.storage.StorageManager
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.provider.Settings
import android.util.Log
import android.view.HapticFeedbackConstants
import android.view.Surface
import android.webkit.MimeTypeMap
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors

/**
 * The native app (AppHost) for this process, and the platform services it calls back into.
 *
 * There is one per process, created by the first activity and never destroyed: the native UI and
 * running jobs outlive any one activity. An activity attaches itself while it exists.
 *
 * The native-to-Kotlin methods at the bottom are called on the native UI thread (see
 * jni/native_host.cpp; proguard-rules.pro keeps them). They never block on the main thread, which
 * may itself be waiting for native code during surface handoff; anything that needs the main
 * thread is posted to it.
 */
class NativeHost private constructor(private val context: Context) {
    private val main = Handler(Looper.getMainLooper())
    private val handle: Long
    private val secrets by lazy { SecretBox(SecretBox::keystoreKey) }
    private val documentReader by lazy { Executors.newSingleThreadExecutor() }
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)

    /** Document trees' calls; native code gets them once (getDocumentTrees) and calls them on I/O threads. */
    val documentTrees = DocumentTrees(context.contentResolver)

    /** Finds security keys for SFTP "sk-" keys; native code gets it once (getSecurityKeys) and calls it on I/O threads. */
    val securityKeys = SecurityKeys(context)

    /** The last metered state sent to native; main thread only. */
    private var metered = connectivity.isActiveNetworkMetered

    private class PendingPick(val request: Long, val maxBytes: Int)

    /** The document pick showing, if any; main thread only. */
    private var pendingPick: PendingPick? = null

    private class PendingSave(val request: Long, val bytes: ByteArray)

    /** The document save showing, if any; main thread only. */
    private var pendingSave: PendingSave? = null

    /** The activity another app asked for a document through, until it has its answer; main thread only. */
    private var requester: DocumentRequestActivity? = null

    /** The request of the folder pick showing, if any; main thread only. */
    private var pendingTree: Long? = null

    /** The attached activity; main thread only. */
    var activity: MainActivity? = null
        set(value) {
            field = value
            value?.applySystemBars(lightSystemBars)
        }

    /** The last setLightSystemBars(), for an activity that attaches later; main thread only. */
    private var lightSystemBars = false

    init {
        handle = nativeCreate(
            context.assets,
            Environment.getExternalStorageDirectory().path,
            context.cacheDir.path,
            context.filesDir.path,
            context.resources.configuration.locales[0].toLanguageTag(),
            versionName(),
            context.packageManager.getPackageInfo(context.packageName, 0).longVersionCode.toString(),
        )
        if (handle == 0L) {
            Log.e(TAG, "The native UI couldn't start; see the native log")
        } else {
            connectivity.registerDefaultNetworkCallback(NetworkWatcher(), main)
        }
    }

    val isRunning get() = handle != 0L

    // --- To native (main thread) ---

    fun surfaceCreated(surface: Surface) { if (isRunning) nativeSurfaceCreated(handle, surface) }
    fun surfaceChanged(width: Int, height: Int) { if (isRunning) nativeSurfaceChanged(handle, width, height) }
    fun surfaceDestroyed() { if (isRunning) nativeSurfaceDestroyed(handle) }
    fun redraw() { if (isRunning) nativeRedraw(handle) }

    fun pointer(count: Int, ints: IntArray, coords: FloatArray, times: LongArray) {
        if (isRunning) nativePointer(handle, count, ints, coords, times)
    }

    fun key(keyCode: Int, action: Int, metaState: Int, repeat: Int, codePoint: Int, timeNs: Long) {
        if (isRunning) nativeKey(handle, keyCode, action, metaState, repeat, codePoint, timeNs)
    }

    fun back() { if (isRunning) nativeBack(handle) }

    /** See EnvironmentUpdate: density, fontScale, safe area (4), IME insets (4), reduced motion, power save, night. */
    fun environment(values: FloatArray) { if (isRunning) nativeEnvironment(handle, values) }

    fun resumed(resumed: Boolean) {
        securityKeys.setResumedActivity(if (resumed) activity else null)
        if (isRunning) nativeResumed(handle, resumed)
    }

    /** Reports the default network as metered or not, and only when that changes. */
    private inner class NetworkWatcher : ConnectivityManager.NetworkCallback() {
        override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
            report(
                !capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED) &&
                    !capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_TEMPORARILY_NOT_METERED),
            )
        }

        // Offline nothing is fetched anyway.
        override fun onLost(network: Network) = report(false)

        private fun report(value: Boolean) {
            if (value == metered) return
            metered = value
            nativeNetwork(handle, value)
        }
    }

    /** UTF-8 text and byte offsets; ImeBridge converts. */
    fun imeValue(
        text: ByteArray, selectionStart: Int, selectionEnd: Int, composingStart: Int, composingEnd: Int, session: Long,
    ) {
        if (isRunning) nativeImeValue(handle, text, selectionStart, selectionEnd, composingStart, composingEnd, session)
    }

    fun imeAction(action: Int, session: Long) { if (isRunning) nativeImeAction(handle, action, session) }

    fun accessibilityEnabled(enabled: Boolean) { if (isRunning) nativeAccessibilityEnabled(handle, enabled) }

    fun requestFullSemantics() { if (isRunning) nativeRequestFullSemantics(handle) }

    fun accessibilityAction(nodeId: Int, action: Int, customId: Int, text: ByteArray?, selectionStart: Int, selectionEnd: Int) {
        if (isRunning) nativeAccessibilityAction(handle, nodeId, action, customId, text, selectionStart, selectionEnd)
    }

    // --- From native (native UI thread) ---

    fun hasAllFilesAccess(): Boolean = Environment.isExternalStorageManager()

    fun requestAllFilesAccess() {
        main.post {
            val packageUri = Uri.fromParts("package", context.packageName, null)
            val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, packageUri)
            if (!startActivity(intent)) {
                startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        }
    }

    fun hasNotificationPermission(): Boolean =
        context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    fun requestNotificationPermission() {
        main.post { activity?.requestNotificationPermission() }
    }

    fun setLightSystemBars(light: Boolean) {
        main.post {
            lightSystemBars = light
            activity?.applySystemBars(light)
        }
    }

    fun openNotificationSettings() {
        main.post {
            startActivity(
                Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName),
            )
        }
    }

    /** Little-endian: u32 count, then per volume str path, str description, u8 primary, u8 removable, str state, u64 total, u64 free. */
    fun storageVolumes(): ByteArray {
        val storage = context.getSystemService(StorageManager::class.java)
        val out = ByteArrayOutputStream()
        val volumes = storage.storageVolumes
        out.write(le(4).putInt(volumes.size).array())
        for (volume in volumes) {
            val directory = volume.directory
            var total = 0L
            var free = 0L
            val mounted = volume.state == Environment.MEDIA_MOUNTED ||
                volume.state == Environment.MEDIA_MOUNTED_READ_ONLY
            if (directory != null && mounted) {
                try {
                    val stat = StatFs(directory.path)
                    total = stat.totalBytes
                    free = stat.availableBytes
                } catch (e: IllegalArgumentException) {
                    Log.w(TAG, "statfs failed for ${directory.path}", e)
                }
            }
            writeString(out, directory?.path ?: "")
            writeString(out, volume.getDescription(context))
            out.write(if (volume.isPrimary) 1 else 0)
            out.write(if (volume.isRemovable) 1 else 0)
            writeString(out, volume.state)
            out.write(le(16).putLong(total).putLong(free).array())
        }
        return out.toByteArray()
    }

    fun openFile(path: String, mimeType: String) {
        val uri = FileContentProvider.grant(path, mimeType)
        val intent = Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, mimeType)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        main.post { startActivity(Intent.createChooser(intent, null)) }
    }

    fun shareFiles(paths: Array<String>, mimeTypes: Array<String>) {
        if (paths.isEmpty()) return
        val uris = ArrayList<Uri>(paths.size)
        for (i in paths.indices) {
            uris += FileContentProvider.grant(paths[i], mimeTypes.getOrElse(i) { "application/octet-stream" })
        }
        val type = commonMimeType(mimeTypes)
        val intent = if (uris.size == 1) {
            Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, uris[0])
        } else {
            Intent(Intent.ACTION_SEND_MULTIPLE).putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
        }
        intent.type = type
        // The chooser and the target only get the grant through ClipData.
        val clip = ClipData.newRawUri(null, uris[0])
        for (i in 1 until uris.size) clip.addItem(ClipData.Item(uris[i]))
        intent.clipData = clip
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        main.post { startActivity(Intent.createChooser(intent, null)) }
    }

    fun setClipboardText(text: String) {
        main.post {
            context.getSystemService(ClipboardManager::class.java)
                .setPrimaryClip(ClipData.newPlainText(null, text))
        }
    }

    /** The clipboard's first item as UTF-8 text, or null. Called on the native UI thread. */
    fun clipboardText(): ByteArray? {
        val clip = context.getSystemService(ClipboardManager::class.java).primaryClip ?: return null
        if (clip.itemCount == 0) return null
        return clip.getItemAt(0).coerceToText(context)?.toString()?.toByteArray(Charsets.UTF_8)
    }

    fun showIme(
        left: Int, top: Int, right: Int, bottom: Int, text: ByteArray, selectionStart: Int,
        selectionEnd: Int, composingStart: Int, composingEnd: Int, keyboard: Int, action: Int,
        flags: Int, session: Long,
    ) {
        val state = ImeState(text, selectionStart, selectionEnd, composingStart, composingEnd)
        val config = ImeConfig(keyboard, action, flags)
        main.post { activity?.imeBridge?.show(state, config, session, left, top, right, bottom) }
    }

    fun hideIme() {
        main.post { activity?.imeBridge?.hide() }
    }

    fun updateIme(
        text: ByteArray, selectionStart: Int, selectionEnd: Int, composingStart: Int, composingEnd: Int, session: Long,
    ) {
        val state = ImeState(text, selectionStart, selectionEnd, composingStart, composingEnd)
        main.post { activity?.imeBridge?.update(state, session) }
    }

    fun updateSemantics(bytes: ByteArray) {
        val update = try {
            SemanticsParser.parse(bytes)
        } catch (e: SemanticsFormatException) {
            Log.e(TAG, "Bad semantics update", e)
            return
        }
        main.post { activity?.accessibility?.apply(update) }
    }

    fun announce(text: String) {
        main.post { activity?.accessibility?.announce(text) }
    }

    fun jobsActive(active: Boolean) {
        main.post {
            if (active) {
                activity?.requestNotificationPermission()
                JobService.start(context)
            } else {
                JobService.stop(context)
            }
        }
    }

    fun updateJobNotification(title: String, text: String, progressPermille: Int) {
        main.post { JobService.update(context, title, text, progressPermille) }
    }

    fun performHaptic(kind: Int) {
        val constant = when (kind) {
            0 -> HapticFeedbackConstants.VIRTUAL_KEY
            1 -> HapticFeedbackConstants.LONG_PRESS
            2 -> HapticFeedbackConstants.CLOCK_TICK
            3 -> HapticFeedbackConstants.CONFIRM
            4 -> HapticFeedbackConstants.REJECT
            5 -> HapticFeedbackConstants.DRAG_START
            else -> return
        }
        main.post { activity?.surfaceView?.performHapticFeedback(constant) }
    }

    fun isNetworkMetered(): Boolean = connectivity.isActiveNetworkMetered

    fun openUrl(url: String) {
        main.post { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
    }

    /** Platform::protectSecret: an AES-GCM blob under the Keystore key, or null. */
    fun protectSecret(plain: ByteArray): ByteArray? = secrets.protect(plain)

    fun unprotectSecret(blob: ByteArray): ByteArray? = secrets.unprotect(blob)

    /**
     * Platform::apkIcon: the app icon of the APK file at [path], drawn into a [sizePx] square, as
     * premultiplied RGBA bytes (row by row, no padding), or null. Unlike the methods around it,
     * this is called on a native I/O thread: parsing the package can take a while.
     */
    fun apkIcon(path: String, sizePx: Int): ByteArray? {
        if (sizePx <= 0) return null
        return try {
            val pm = context.packageManager
            val info = pm.getPackageArchiveInfo(path, PackageManager.PackageInfoFlags.of(0))
                ?.applicationInfo ?: return null
            // The package isn't installed: its resources are in the file.
            info.sourceDir = path
            info.publicSourceDir = path
            val icon = info.loadIcon(pm) ?: return null
            val bitmap = Bitmap.createBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888)
            // An adaptive icon draws its own background, masked to the system's shape.
            icon.setBounds(0, 0, sizePx, sizePx)
            icon.draw(Canvas(bitmap))
            val pixels = ByteBuffer.allocate(bitmap.byteCount)
            bitmap.copyPixelsToBuffer(pixels)
            bitmap.recycle()
            pixels.array()
        } catch (e: Exception) {
            Log.w(TAG, "No icon for $path", e)
            null
        }
    }

    /**
     * Platform::pickDocument: the system picker (ACTION_OPEN_DOCUMENT) for one document. The answer
     * goes back through nativeDocumentPicked, exactly once: the bytes (at most [maxBytes]), or why
     * there are none. A newer pick replaces one still showing; native code has already given up on
     * the older one.
     */
    fun pickDocument(request: Long, mimeTypes: Array<String>, maxBytes: Int) {
        main.post {
            pendingPick?.let { answerPick(it.request, PICK_CANCELLED) }
            val activity = activity
            if (activity == null || !activity.pickDocument(mimeTypes)) {
                pendingPick = null
                answerPick(request, PICK_FAILED)
                return@post
            }
            pendingPick = PendingPick(request, maxBytes)
        }
    }

    /** From [MainActivity.onActivityResult]: the picked document, or null if backed out. Main thread. */
    fun documentResult(uri: Uri?) {
        val pick = pendingPick ?: return
        pendingPick = null
        if (uri == null) {
            answerPick(pick.request, PICK_CANCELLED)
            return
        }
        // Read off the main thread; the grant to uri lasts while the activity does, and isn't kept.
        documentReader.execute { readPicked(pick, uri) }
    }

    private fun readPicked(pick: PendingPick, uri: Uri) {
        val resolver = context.contentResolver
        val name = try {
            resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getString(0) else null
            }
        } catch (e: RuntimeException) {
            Log.w(TAG, "No display name for a picked document", e)
            null
        }
        try {
            val input = resolver.openInputStream(uri)
            if (input == null) {
                answerPick(pick.request, PICK_FAILED, name)
                return
            }
            input.use {
                val out = ByteArrayOutputStream()
                val buffer = ByteArray(8192)
                while (true) {
                    val read = it.read(buffer)
                    if (read < 0) break
                    if (out.size() + read > pick.maxBytes) {
                        answerPick(pick.request, PICK_TOO_LARGE, name)
                        return
                    }
                    out.write(buffer, 0, read)
                }
                val bytes = out.toByteArray()
                answerPick(pick.request, PICK_PICKED, name, bytes)
                // It may be key material: don't leave a copy lying around longer than needed.
                bytes.fill(0)
                buffer.fill(0)
            }
        } catch (e: IOException) {
            Log.w(TAG, "Couldn't read a picked document", e)
            answerPick(pick.request, PICK_FAILED, name)
        } catch (e: SecurityException) {
            Log.w(TAG, "No access to a picked document", e)
            answerPick(pick.request, PICK_FAILED, name)
        }
    }

    /**
     * Platform::saveDocument: the system picker (ACTION_CREATE_DOCUMENT) for where to save one
     * document, then [bytes] written there. The answer goes back through nativeDocumentSaved,
     * exactly once. A newer save replaces one still showing.
     */
    fun saveDocument(request: Long, name: String, mimeType: String, bytes: ByteArray) {
        main.post {
            pendingSave?.let { cancelled ->
                cancelled.bytes.fill(0)
                answerSave(cancelled.request, SAVE_CANCELLED)
            }
            val activity = activity
            if (activity == null || !activity.createDocument(name, mimeType)) {
                pendingSave = null
                bytes.fill(0)
                answerSave(request, SAVE_FAILED)
                return@post
            }
            pendingSave = PendingSave(request, bytes)
        }
    }

    /** From [MainActivity.onActivityResult]: where to save, or null if backed out. Main thread. */
    fun createDocumentResult(uri: Uri?) {
        val save = pendingSave ?: return
        pendingSave = null
        if (uri == null) {
            save.bytes.fill(0)
            answerSave(save.request, SAVE_CANCELLED)
            return
        }
        documentReader.execute { writeSaved(save, uri) }
    }

    private fun writeSaved(save: PendingSave, uri: Uri) {
        val resolver = context.contentResolver
        try {
            val output = resolver.openOutputStream(uri, "wt") ?: throw IOException("No stream")
            output.use { it.write(save.bytes) }
            answerSave(save.request, SAVE_SAVED, displayName(uri))
        } catch (e: IOException) {
            Log.w(TAG, "Couldn't write a saved document", e)
            deleteDocument(uri)
            answerSave(save.request, SAVE_FAILED)
        } catch (e: SecurityException) {
            Log.w(TAG, "No access to a saved document", e)
            deleteDocument(uri)
            answerSave(save.request, SAVE_FAILED)
        } finally {
            // It may hold sealed secrets: don't leave a copy lying around.
            save.bytes.fill(0)
        }
    }

    /** The empty document a failed save left behind. */
    private fun deleteDocument(uri: Uri) {
        try {
            DocumentsContract.deleteDocument(context.contentResolver, uri)
        } catch (e: Exception) {
            Log.w(TAG, "Couldn't remove a document that wasn't written", e)
        }
    }

    private fun displayName(uri: Uri): String? = try {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getString(0) else null
        }
    } catch (e: RuntimeException) {
        Log.w(TAG, "No display name for a document", e)
        null
    }

    private fun answerSave(request: Long, status: Int, name: String? = null) {
        if (isRunning) nativeDocumentSaved(handle, request, status, name)
    }

    /** Any thread: AppHost posts it to the UI thread. */
    /**
     * Platform::openDocumentTree: the system picker for a folder to keep (ACTION_OPEN_DOCUMENT_TREE).
     * The answer goes back through nativeDocumentTreePicked, exactly once, with a persistable grant
     * to the folder taken.
     */
    fun openDocumentTree(request: Long) {
        main.post {
            pendingTree?.let { answerTree(it, PICK_CANCELLED) }
            val activity = activity
            if (activity == null || !activity.openDocumentTree()) {
                pendingTree = null
                answerTree(request, PICK_FAILED)
                return@post
            }
            pendingTree = request
        }
    }

    /** From [MainActivity.onActivityResult]: the picked folder, or null if backed out. Main thread. */
    fun documentTreeResult(uri: Uri?) {
        val request = pendingTree ?: return
        pendingTree = null
        if (uri == null) {
            answerTree(request, PICK_CANCELLED)
            return
        }
        documentReader.execute {
            try {
                context.contentResolver.takePersistableUriPermission(uri, TREE_GRANT)
            } catch (e: SecurityException) {
                Log.w(TAG, "No persistable grant to a picked folder", e)
                answerTree(request, PICK_FAILED)
                return@execute
            }
            answerTree(request, PICK_PICKED, uri.toString(), documentTrees.displayName(uri.toString()))
        }
    }

    /** Platform::releaseDocumentTree. */
    fun releaseDocumentTree(uri: String) {
        try {
            context.contentResolver.releasePersistableUriPermission(Uri.parse(uri), TREE_GRANT)
        } catch (e: SecurityException) {
            Log.w(TAG, "No grant to release", e)
        }
    }

    private fun answerTree(request: Long, status: Int, uri: String? = null, name: String? = null) {
        if (isRunning) nativeDocumentTreePicked(handle, request, status, uri, name)
    }

    private fun answerPick(request: Long, status: Int, name: String? = null, bytes: ByteArray? = null) {
        if (isRunning) nativeDocumentPicked(handle, request, status, name, bytes)
    }

    /**
     * Another app asking for a document, from [activity] on the main thread: a new one to write
     * (ACTION_CREATE_DOCUMENT), or files to read (ACTION_GET_CONTENT, ACTION_OPEN_DOCUMENT). An
     * older request still waiting is cancelled: its activity goes.
     */
    fun receiveDocumentRequest(activity: DocumentRequestActivity, intent: Intent) {
        if (!isRunning) {
            activity.finish()
            return
        }
        requester?.takeIf { it !== activity }?.finish()
        requester = activity
        if (intent.action == Intent.ACTION_CREATE_DOCUMENT) {
            val type = intent.type?.takeUnless { it.isEmpty() || '*' in it } ?: "application/octet-stream"
            val fallback = "Untitled" + (MimeTypeMap.getSingleton().getExtensionFromMimeType(type)?.let { ".$it" } ?: "")
            val name = safeFileName(intent.getStringExtra(Intent.EXTRA_TITLE), fallback)
            nativeDocumentRequested(handle, REQUEST_CREATE, name, arrayOf(type), false)
        } else {
            // EXTRA_MIME_TYPES narrows a broad type ("*/*" with several types to choose from).
            val types = intent.getStringArrayExtra(Intent.EXTRA_MIME_TYPES)?.filterNotNull()?.takeIf { it.isNotEmpty() }
                ?: listOfNotNull(intent.type)
            val multiple = intent.getBooleanExtra(Intent.EXTRA_ALLOW_MULTIPLE, false)
            nativeDocumentRequested(handle, REQUEST_PICK, null, types.map { it.lowercase() }.toTypedArray(), multiple)
        }
    }

    /** [activity] is going; if it still waited for its answer, native code drops the request. Main thread. */
    fun documentRequestEnded(activity: DocumentRequestActivity) {
        if (requester !== activity) return
        requester = null
        if (isRunning) nativeDocumentRequested(handle, REQUEST_ENDED, null, null, false)
    }

    /**
     * The answer for the app that asked for a document: [paths] served through
     * [FileContentProvider] (writable for one it creates), or none when the user cancelled. The
     * asking activity returns them, several as ClipData, and finishes.
     */
    fun answerDocumentRequest(paths: Array<String>, mimeTypes: Array<String>, writable: Boolean) {
        main.post {
            val activity = requester ?: return@post
            requester = null
            if (paths.isEmpty()) {
                activity.setResult(Activity.RESULT_CANCELED)
            } else {
                val uris = paths.mapIndexed { i, path ->
                    FileContentProvider.grant(path, mimeTypes.getOrElse(i) { "application/octet-stream" }, writable)
                }
                var flags = Intent.FLAG_GRANT_READ_URI_PERMISSION
                if (writable) flags = flags or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                val result = Intent().setData(uris[0]).addFlags(flags)
                if (uris.size > 1) {
                    val clip = ClipData.newRawUri(null, uris[0])
                    for (uri in uris.drop(1)) clip.addItem(ClipData.Item(uri))
                    result.clipData = clip
                }
                activity.setResult(Activity.RESULT_OK, result)
            }
            activity.finish()
        }
    }

    /**
     * What another app shared (ACTION_SEND or ACTION_SEND_MULTIPLE), from [MainActivity] on the
     * main thread. Each document is opened at once and its descriptor handed to native code: the
     * read grant ends with the activity, an open descriptor doesn't. Text is shared as a .txt file
     * when there are no documents.
     */
    fun receiveShare(intent: Intent, fromOtherApp: Boolean) {
        if (!isRunning) return
        val streams = when (intent.action) {
            Intent.ACTION_SEND -> listOfNotNull(intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java))
            Intent.ACTION_SEND_MULTIPLE ->
                intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java).orEmpty().filterNotNull()
            else -> return
        }.take(MAX_SHARED)
        val type = intent.type
        val text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT)?.toString()
        val subject = intent.getStringExtra(Intent.EXTRA_SUBJECT)
        documentReader.execute { openShared(streams, type, text, subject, fromOtherApp) }
    }

    private fun openShared(streams: List<Uri>, type: String?, text: String?, subject: String?, fromOtherApp: Boolean) {
        val resolver = context.contentResolver
        val fds = ArrayList<Int>()
        val names = ArrayList<String>()
        val sizes = ArrayList<Long>()
        for (uri in streams) {
            val fd = try {
                resolver.openFileDescriptor(uri, "r")?.detachFd()
            } catch (e: Exception) {
                // FileNotFoundException, SecurityException, IllegalArgumentException: nothing to save.
                Log.w(TAG, "Couldn't open a shared document", e)
                null
            } ?: continue
            val (name, size) = describeShared(uri)
            val fallback = "shared" + (
                MimeTypeMap.getSingleton().getExtensionFromMimeType(resolver.getType(uri) ?: type)
                    ?.let { ".$it" } ?: ""
                )
            fds += fd
            names += safeFileName(name ?: uri.lastPathSegment, fallback)
            sizes += size ?: -1L
        }
        var textBytes: ByteArray? = null
        var textName: String? = null
        if (fds.isEmpty() && text != null) {
            textBytes = text.toByteArray(Charsets.UTF_8)
            textName = safeFileName(subject?.take(100), "Shared text") + ".txt"
        }
        if (fds.isEmpty() && textBytes == null) {
            Log.w(TAG, "Nothing to save in a share")
            return
        }
        nativeShareReceived(
            handle, fds.toIntArray(), names.toTypedArray(), sizes.toLongArray(), textBytes, textName, fromOtherApp,
        )
    }

    /** The shared document's display name and size, where its provider gives them. */
    private fun describeShared(uri: Uri): Pair<String?, Long?> = try {
        context.contentResolver.query(
            uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null,
        )?.use { cursor ->
            if (!cursor.moveToFirst()) return@use null
            val name = if (cursor.isNull(0)) null else cursor.getString(0)
            val size = if (cursor.isNull(1)) null else cursor.getLong(1).takeIf { it >= 0 }
            name to size
        } ?: (null to null)
    } catch (e: RuntimeException) {
        Log.w(TAG, "No name for a shared document", e)
        null to null
    }

    /** A name that can be a file: no '/' or NUL, and not empty, "." or "..". */
    private fun safeFileName(name: String?, fallback: String): String {
        val cleaned = name?.replace('/', '_')?.replace('\u0000', '_')?.trim().orEmpty()
        return if (cleaned.isEmpty() || cleaned == "." || cleaned == "..") fallback else cleaned
    }

    fun finishActivity() {
        main.post {
            val activity = activity ?: return@post
            // What back does at the root since Android 12: keep the task, just leave it.
            if (activity.isTaskRoot) activity.moveTaskToBack(false) else activity.finish()
        }
    }

    // --- Helpers ---

    private fun startActivity(intent: Intent): Boolean {
        val activity = activity
        return try {
            if (activity != null) {
                activity.startActivity(intent)
            } else {
                context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            }
            true
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No activity for $intent", e)
            false
        }
    }

    private fun versionName(): String =
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: ""

    private fun le(size: Int) = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)

    private fun writeString(out: ByteArrayOutputStream, value: String) {
        val bytes = Utf8.encode(value)
        out.write(le(4).putInt(bytes.size).array())
        out.write(bytes)
    }

    private fun commonMimeType(mimeTypes: Array<String>): String {
        val first = mimeTypes.firstOrNull() ?: return "*/*"
        if (mimeTypes.all { it == first }) return first
        val topLevel = first.substringBefore('/')
        return if (mimeTypes.all { it.substringBefore('/') == topLevel }) "$topLevel/*" else "*/*"
    }

    private external fun nativeCreate(assets: android.content.res.AssetManager, homePath: String, cacheDir: String, filesDir: String, locale: String, version: String, build: String): Long
    private external fun nativeSurfaceCreated(handle: Long, surface: Surface)
    private external fun nativeSurfaceChanged(handle: Long, width: Int, height: Int)
    private external fun nativeSurfaceDestroyed(handle: Long)
    private external fun nativeRedraw(handle: Long)
    private external fun nativePointer(handle: Long, count: Int, ints: IntArray, coords: FloatArray, times: LongArray)
    private external fun nativeKey(handle: Long, keyCode: Int, action: Int, metaState: Int, repeat: Int, codePoint: Int, timeNs: Long)
    private external fun nativeBack(handle: Long)
    private external fun nativeEnvironment(handle: Long, values: FloatArray)
    private external fun nativeResumed(handle: Long, resumed: Boolean)
    private external fun nativeNetwork(handle: Long, metered: Boolean)
    private external fun nativeImeValue(
        handle: Long, text: ByteArray, selectionStart: Int, selectionEnd: Int, composingStart: Int, composingEnd: Int,
        session: Long,
    )
    private external fun nativeImeAction(handle: Long, action: Int, session: Long)
    private external fun nativeAccessibilityEnabled(handle: Long, enabled: Boolean)
    private external fun nativeRequestFullSemantics(handle: Long)
    private external fun nativeAccessibilityAction(handle: Long, nodeId: Int, action: Int, customId: Int, text: ByteArray?, selectionStart: Int, selectionEnd: Int)
    private external fun nativeDocumentPicked(handle: Long, request: Long, status: Int, name: String?, bytes: ByteArray?)
    private external fun nativeDocumentSaved(handle: Long, request: Long, status: Int, name: String?)
    private external fun nativeDocumentTreePicked(handle: Long, request: Long, status: Int, uri: String?, name: String?)
    private external fun nativeDocumentRequested(
        handle: Long, kind: Int, name: String?, mimeTypes: Array<String>?, multiple: Boolean,
    )
    private external fun nativeShareReceived(
        handle: Long, fds: IntArray, names: Array<String>, sizes: LongArray, text: ByteArray?, textName: String?,
        fromOtherApp: Boolean,
    )

    companion object {
        private const val TAG = "FilePan"

        // runtime::PickedDocument::Status.
        private const val PICK_PICKED = 0
        private const val PICK_CANCELLED = 1
        private const val PICK_FAILED = 2
        private const val PICK_TOO_LARGE = 3
        private const val TREE_GRANT = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        private const val SAVE_SAVED = 0
        private const val SAVE_CANCELLED = 1
        private const val SAVE_FAILED = 2

        // runtime::DocumentRequest::Kind.
        private const val REQUEST_CREATE = 0
        private const val REQUEST_PICK = 1
        private const val REQUEST_ENDED = 2

        /** At most this many shared documents are opened, well under the descriptor limit. */
        private const val MAX_SHARED = 1000

        private var instance: NativeHost? = null

        /** Main thread. */
        fun get(context: Context): NativeHost {
            instance?.let { return it }
            System.loadLibrary("filepan")
            return NativeHost(context.applicationContext).also { instance = it }
        }
    }
}

/** IME state from native code: UTF-8 text and byte offsets (-1 for none). */
class ImeState(
    val text: ByteArray,
    val selectionStart: Int,
    val selectionEnd: Int,
    val composingStart: Int,
    val composingEnd: Int,
)

/** ui::TextInputConfig: KeyboardType and ImeAction ordinals, and flags (1 multiline). */
class ImeConfig(val keyboard: Int, val action: Int, val flags: Int)
