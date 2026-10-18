// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.utils

import android.app.ActivityManager
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.hardware.display.DisplayManager
import android.net.Uri
import android.os.Build
import android.view.Display
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.widget.Toast
import androidx.documentfile.provider.DocumentFile
import androidx.fragment.app.FragmentActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.citra.citra_emu.BuildConfig
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R

/**
 * Claudhar's Phase 0 device probe. It records what the rebuild's design depends on:
 * the identity strings a known-device table would match, every display (which one
 * can be the bottom screen), every input device (the built-in controller's IDs and
 * buttons), and the native CPU and Vulkan measurements. The plain-text report is
 * saved as log/claudhar_probe.txt in the user folder and can be shared or copied.
 */
object DeviceProbe {
    private const val FILE_NAME = "claudhar_probe.txt"

    /** Gamepad keys whose presence the report lists for each input device. */
    private val gamepadKeys = intArrayOf(
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_THUMBL,
        KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_START,
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_BACK
    )

    /**
     * Runs the probe on a background thread behind a progress dialog, saves the report
     * and shows it with Share and Copy buttons. Called from the home screen while no
     * game is running.
     */
    fun start(activity: FragmentActivity) {
        val progress = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.device_probe)
            .setMessage(R.string.device_probe_running)
            .setCancelable(false)
            .show()
        val context = activity.applicationContext
        Thread {
            val report = run(context)
            val saved = save(context, FILE_NAME, report)
            activity.runOnUiThread {
                progress.dismiss()
                if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
                MaterialAlertDialogBuilder(activity)
                    .setTitle(R.string.device_probe)
                    .setMessage(report)
                    .setPositiveButton(R.string.device_probe_share) { _, _ ->
                        share(activity, saved, report, R.string.device_probe)
                    }
                    .setNeutralButton(R.string.device_probe_copy) { _, _ ->
                        copy(activity, report)
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            }
        }.start()
    }

    /** Builds the whole report. Takes several seconds, so never on the main thread. */
    fun run(context: Context): String = identity(context) + displays(context) + inputs() +
        NativeLibrary.runDeviceProbe()

    /** Identity strings, Android version and memory. */
    private fun identity(context: Context): String = buildString {
        appendLine("== Device ==")
        appendLine("manufacturer: ${Build.MANUFACTURER}  brand: ${Build.BRAND}")
        appendLine("model: ${Build.MODEL}  device: ${Build.DEVICE}  product: ${Build.PRODUCT}")
        appendLine("board: ${Build.BOARD}  hardware: ${Build.HARDWARE}")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            appendLine("soc: ${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
        }
        appendLine("android: ${Build.VERSION.RELEASE} (sdk ${Build.VERSION.SDK_INT})")
        appendLine("abis: ${Build.SUPPORTED_ABIS.joinToString()}")
        appendLine("fingerprint: ${Build.FINGERPRINT}")
        val memory = ActivityManager.MemoryInfo()
        context.getSystemService(ActivityManager::class.java).getMemoryInfo(memory)
        appendLine("memory: ${memory.totalMem shr 20} MiB, ${memory.availMem shr 20} MiB free")
        appendLine("app: ${BuildConfig.APPLICATION_ID} ${BuildConfig.VERSION_NAME}")
    }

    /**
     * Every display with its modes. "secondary candidate" applies the same rule as
     * Azahar's SecondaryDisplay, which decides where the bottom screen can go.
     */
    private fun displays(context: Context): String = buildString {
        appendLine("== Displays ==")
        val manager = context.getSystemService(DisplayManager::class.java)
        val presentation = manager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
            .map { it.displayId }
        for (display in manager.displays) {
            val mode = display.mode
            val modes = display.supportedModes.joinToString {
                "${it.physicalWidth}x${it.physicalHeight}@${"%.0f".format(it.refreshRate)}"
            }
            val candidate = (
                display.displayId != Display.DEFAULT_DISPLAY ||
                    display.displayId in presentation
                ) &&
                display.name != "HiddenDisplay" && display.state != Display.STATE_OFF &&
                display.isValid
            appendLine(
                "display ${display.displayId}: \"${display.name}\" " +
                    "${mode.physicalWidth}x${mode.physicalHeight} " +
                    "${"%.1f".format(display.refreshRate)} Hz, flags=${display.flags}, " +
                    "state=${display.state}, presentation=${display.displayId in presentation}, " +
                    "secondary candidate=$candidate"
            )
            appendLine("  modes: $modes")
        }
    }

    /** Every input device: name, IDs, source types, gamepad keys and axes. */
    private fun inputs(): String = buildString {
        appendLine("== Input devices ==")
        for (id in InputDevice.getDeviceIds()) {
            val device = InputDevice.getDevice(id) ?: continue
            val keys = device.hasKeys(*gamepadKeys)
            val present = gamepadKeys.filterIndexed { i, _ -> keys[i] }
                .joinToString(" ") { KeyEvent.keyCodeToString(it).removePrefix("KEYCODE_") }
            val axes = device.motionRanges.map { it.axis }.distinct()
                .joinToString(" ") { MotionEvent.axisToString(it).removePrefix("AXIS_") }
            appendLine(
                "input $id: \"${device.name}\" vendor=0x%04x product=0x%04x ".format(
                    device.vendorId,
                    device.productId
                ) + "external=${device.isExternal} sources=0x%08x".format(device.sources)
            )
            appendLine("  keys: $present")
            appendLine("  axes: $axes")
        }
    }

    /**
     * Writes a text report to log/[name] in the user folder, replacing an older one, and
     * returns its URI, or null if it can't. Also used for crash reports (CrashReport).
     */
    internal fun save(context: Context, name: String, text: String): Uri? = try {
        val root = DocumentFile.fromTreeUri(context, PermissionsHandler.citraDirectory)
        val log = root?.findFile("log") ?: root?.createDirectory("log")
        log?.findFile(name)?.delete()
        log?.createFile("text/plain", name)?.uri?.also { uri ->
            context.contentResolver.openOutputStream(uri)?.use { it.write(text.toByteArray()) }
        }
    } catch (e: Exception) {
        Log.error("[DeviceProbe] Could not save $name: $e")
        null
    }

    /** Shares a saved report, or the text itself when it could not be saved. */
    internal fun share(activity: FragmentActivity, saved: Uri?, text: String, title: Int) {
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            if (saved != null) {
                putExtra(Intent.EXTRA_STREAM, saved)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } else {
                putExtra(Intent.EXTRA_TEXT, text)
            }
        }
        activity.startActivity(Intent.createChooser(intent, activity.getText(title)))
    }

    /** Copies the report to the clipboard. */
    private fun copy(activity: FragmentActivity, report: String) {
        activity.getSystemService(ClipboardManager::class.java)
            .setPrimaryClip(ClipData.newPlainText("Claudhar device probe", report))
        Toast.makeText(activity, R.string.device_probe_copied, Toast.LENGTH_SHORT).show()
    }
}
