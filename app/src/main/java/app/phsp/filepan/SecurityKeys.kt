// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

package app.phsp.filepan

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.nfc.NfcAdapter
import android.nfc.Tag
import android.nfc.tech.IsoDep
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * Finds the security key the user presents while an SFTP login signs with one: a FIDO key for an
 * "sk-" key, or an OpenPGP card (a YubiKey's, say) for a card login. One plugged in over USB, once
 * the user lets the app use it, or one held to the phone over NFC. It only finds keys and moves
 * bytes: jni/security_keys.cpp speaks CTAPHID or CCID over the USB connection's file descriptor,
 * and ISO 7816 through [transceive].
 *
 * Native code calls [start], [stop], [transceive] and [closeKey] on its I/O threads (proguard-rules.pro
 * keeps them); a key found is handed over on a thread of its own, as opening it talks to it.
 */
class SecurityKeys(private val context: Context) {
    private val main = Handler(Looper.getMainLooper())
    private val opener = Executors.newSingleThreadExecutor()
    private val usb = context.getSystemService(UsbManager::class.java)
    private val nfc: NfcAdapter? = NfcAdapter.getDefaultAdapter(context)
    private val nextToken = AtomicInteger(1)
    private val keys = ConcurrentHashMap<Int, Key>()

    /** The native source looking for a key, or 0 while none is; set on the main thread. */
    @Volatile
    private var source = 0L

    /** Whether the source wants an OpenPGP card, which is behind a CCID interface over USB. */
    @Volatile
    private var card = false

    /** The resumed activity, which NFC reader mode needs; main thread only. */
    private var activity: Activity? = null
    private var readerModeOn: Activity? = null
    private var receiving = false

    private sealed interface Key {
        fun close()
    }

    private class UsbKey(val connection: UsbDeviceConnection, val usbInterface: UsbInterface) : Key {
        override fun close() {
            connection.releaseInterface(usbInterface)
            connection.close()
        }
    }

    private class NfcKey(val isoDep: IsoDep) : Key {
        override fun close() {
            try {
                isoDep.close()
            } catch (_: IOException) {
            }
        }
    }

    // --- From native (I/O threads) ---

    fun start(source: Long, card: Boolean) {
        main.post {
            this.source = source
            this.card = card
            if (usb != null && !receiving) {
                context.registerReceiver(
                    usbReceiver,
                    IntentFilter().apply {
                        addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                        addAction(permissionAction)
                    },
                    Context.RECEIVER_NOT_EXPORTED,
                )
                receiving = true
                usb.deviceList.values.forEach(::offerUsb)
            }
            updateReaderMode()
        }
    }

    fun stop() {
        main.post {
            source = 0
            if (receiving) {
                context.unregisterReceiver(usbReceiver)
                receiving = false
            }
            updateReaderMode()
        }
    }

    /** Sends an APDU to the NFC key [token]; null if the tag was lost or closed. */
    fun transceive(token: Int, apdu: ByteArray): ByteArray? {
        val key = keys[token] as? NfcKey ?: return null
        return try {
            key.isoDep.transceive(apdu)
        } catch (e: IOException) {
            null
        } catch (e: SecurityException) {
            // The tag went out of date: it left the field and came back.
            null
        }
    }

    /** Closes key [token], stopping a transceive in progress; again does nothing. */
    fun closeKey(token: Int) {
        keys.remove(token)?.close()
    }

    // --- Activity (main thread) ---

    /** The activity resumed, or null when it pauses: NFC reader mode needs one in front. */
    fun setResumedActivity(activity: Activity?) {
        this.activity = activity
        updateReaderMode()
    }

    // --- USB ---

    private val permissionAction = "${context.packageName}.securitykey.USB_PERMISSION"

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java) ?: return
            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> offerUsb(device)
                permissionAction ->
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) offerUsb(device)
            }
        }
    }

    private fun offerUsb(device: UsbDevice) {
        val usb = usb ?: return
        if (source == 0L) return
        val card = card
        val usbInterface = (0 until device.interfaceCount).map(device::getInterface)
            .firstOrNull {
                if (card) {
                    it.interfaceClass == UsbConstants.USB_CLASS_CSCID && cardEndpoints(it) != null
                } else {
                    it.interfaceClass == UsbConstants.USB_CLASS_HID && endpoints(it) != null
                }
            }
            ?: return
        if (!usb.hasPermission(device)) {
            val intent = Intent(permissionAction).setPackage(context.packageName)
            usb.requestPermission(
                device,
                PendingIntent.getBroadcast(context, 0, intent, PendingIntent.FLAG_MUTABLE),
            )
            return
        }
        val (inEndpoint, outEndpoint) = if (card) cardEndpoints(usbInterface)!! else endpoints(usbInterface)!!
        val connection = usb.openDevice(device) ?: return
        if (!connection.claimInterface(usbInterface, true)) {
            connection.close()
            return
        }
        val token = nextToken.getAndIncrement()
        keys[token] = UsbKey(connection, usbInterface)
        val source = source
        opener.execute {
            if (card) {
                nativeUsbCard(
                    source, connection.fileDescriptor, inEndpoint.address, outEndpoint.address,
                    inEndpoint.maxPacketSize, token,
                )
            } else {
                nativeUsbKey(
                    source, connection.fileDescriptor, inEndpoint.address, outEndpoint.address,
                    outEndpoint.maxPacketSize, token,
                )
            }
        }
    }

    /** A FIDO key's HID interface has interrupt endpoints both ways with 64-byte reports. */
    private fun endpoints(usbInterface: UsbInterface): Pair<UsbEndpoint, UsbEndpoint>? {
        val endpoints = (0 until usbInterface.endpointCount).map(usbInterface::getEndpoint)
            .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_INT && it.maxPacketSize == 64 }
        val inEndpoint = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_IN } ?: return null
        val outEndpoint = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT } ?: return null
        return inEndpoint to outEndpoint
    }

    /** A CCID interface (a smart card reader, or a YubiKey's own) has bulk endpoints both ways. */
    private fun cardEndpoints(usbInterface: UsbInterface): Pair<UsbEndpoint, UsbEndpoint>? {
        val endpoints = (0 until usbInterface.endpointCount).map(usbInterface::getEndpoint)
            .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        val inEndpoint = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_IN } ?: return null
        val outEndpoint = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT } ?: return null
        return inEndpoint to outEndpoint
    }

    // --- NFC ---

    private fun updateReaderMode() {
        val nfc = nfc ?: return
        val wanted = if (source != 0L) activity else null
        if (wanted === readerModeOn) return
        readerModeOn?.let {
            try {
                nfc.disableReaderMode(it)
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Couldn't stop NFC reader mode", e)
            }
        }
        readerModeOn = null
        if (wanted != null) {
            try {
                nfc.enableReaderMode(
                    wanted, ::onTag,
                    NfcAdapter.FLAG_READER_NFC_A or NfcAdapter.FLAG_READER_NFC_B or
                        NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK,
                    null,
                )
                readerModeOn = wanted
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Couldn't start NFC reader mode", e)
            }
        }
    }

    /** On a binder thread. */
    private fun onTag(tag: Tag) {
        val source = source
        if (source == 0L) return
        val isoDep = IsoDep.get(tag)
        val token = nextToken.getAndIncrement()
        if (isoDep == null) {
            opener.execute { nativeNfcKey(source, 0, false) }
            return
        }
        try {
            isoDep.connect()
            isoDep.timeout = NFC_TIMEOUT_MILLIS
        } catch (e: IOException) {
            return
        }
        keys[token] = NfcKey(isoDep)
        opener.execute { nativeNfcKey(source, token, isoDep.isExtendedLengthApduSupported) }
    }

    /** Hands over a USB key whose interface is claimed; closeKey(token) releases it. */
    private external fun nativeUsbKey(
        source: Long,
        fd: Int,
        inEndpoint: Int,
        outEndpoint: Int,
        packetSize: Int,
        token: Int,
    )

    /** Hands over an OpenPGP card's CCID interface, claimed; closeKey(token) releases it. */
    private external fun nativeUsbCard(
        source: Long,
        fd: Int,
        inEndpoint: Int,
        outEndpoint: Int,
        packetSize: Int,
        token: Int,
    )

    /** Hands over an NFC key, connected; token 0 for a tag that isn't one. */
    private external fun nativeNfcKey(source: Long, token: Int, extendedLength: Boolean)

    companion object {
        private const val TAG = "SecurityKeys"
        private const val NFC_TIMEOUT_MILLIS = 30_000
    }
}
