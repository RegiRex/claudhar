// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.utils

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import androidx.annotation.RequiresApi
import androidx.fragment.app.FragmentActivity
import androidx.preference.PreferenceManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.util.Date
import org.citra.citra_emu.BuildConfig
import org.citra.citra_emu.R

/**
 * Claudhar: reports the last crash without needing a computer. When Claudhar crashes,
 * Android keeps a record of it; for a native crash that record (a tombstone) holds the
 * signal, the crashing thread's backtrace and recent log lines. On the next launch this
 * reads the record, decodes it to text, saves it as log/claudhar_crash.txt in the user
 * folder and offers to share it, once per crash. Backtrace addresses are turned into
 * function names afterwards with the build's native-debug-symbols artifact.
 */
object CrashReport {
    private const val FILE_NAME = "claudhar_crash.txt"
    private const val LAST_REPORTED = "claudhar_last_reported_crash"
    private var checked = false

    /**
     * Checks once per app process, on the main thread, after the user folder is
     * available. The lookup and decoding run on a background thread.
     */
    fun offer(activity: FragmentActivity) {
        if (checked || Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        checked = true
        val context = activity.applicationContext
        Thread {
            val report = describeLastCrash(context) ?: return@Thread
            val saved = DeviceProbe.save(context, FILE_NAME, report)
            activity.runOnUiThread {
                if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
                MaterialAlertDialogBuilder(activity)
                    .setTitle(R.string.crash_report)
                    .setMessage(R.string.crash_report_description)
                    .setPositiveButton(R.string.device_probe_share) { _, _ ->
                        DeviceProbe.share(activity, saved, report, R.string.crash_report)
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
            }
        }.start()
    }

    /** The last exit as text if it was a crash not reported yet, otherwise null. */
    @RequiresApi(Build.VERSION_CODES.R)
    private fun describeLastCrash(context: Context): String? {
        val exit = context.getSystemService(ActivityManager::class.java)
            .getHistoricalProcessExitReasons(context.packageName, 0, 1).firstOrNull()
            ?: return null
        val kind = when (exit.reason) {
            ApplicationExitInfo.REASON_CRASH_NATIVE -> "native crash"
            ApplicationExitInfo.REASON_CRASH -> "Java crash"
            ApplicationExitInfo.REASON_ANR -> "not responding (ANR)"
            else -> return null
        }
        val preferences = PreferenceManager.getDefaultSharedPreferences(context)
        if (preferences.getLong(LAST_REPORTED, 0L) >= exit.timestamp) return null
        preferences.edit().putLong(LAST_REPORTED, exit.timestamp).apply()
        val trace = runCatching { exit.traceInputStream?.use { it.readBytes() } }.getOrNull()
        return buildString {
            appendLine("== Claudhar crash report ==")
            appendLine(
                "app: ${BuildConfig.APPLICATION_ID} ${BuildConfig.VERSION_NAME} " +
                    "(${BuildConfig.GIT_HASH})"
            )
            appendLine("time: ${Date(exit.timestamp)}  kind: $kind  status: ${exit.status}")
            appendLine("description: ${exit.description}")
            appendLine("memory: pss ${exit.pss shr 10} MiB, rss ${exit.rss shr 10} MiB")
            when {
                trace == null -> appendLine("Android kept no trace for this exit.")

                exit.reason == ApplicationExitInfo.REASON_CRASH_NATIVE &&
                    Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
                    // A malformed record must not crash the report itself.
                    append(runCatching { Tombstone(trace).toString() }.getOrElse { "$it\n" })

                else -> append(String(trace))
            }
        }
    }

    /**
     * Decodes the parts of Android's tombstone protobuf (system/core/debuggerd/proto/
     * tombstone.proto) that locate a crash: the signal, the abort message, every thread's
     * name, the crashing thread's backtrace and the last lines of each log buffer.
     */
    private class Tombstone(private val bytes: ByteArray) {
        /** One protobuf field: its number, a varint or fixed value, or a byte range. */
        private data class Field(val number: Int, val value: Long, val start: Int, val end: Int)

        /** The fields of the message in bytes [from, to). Stops at an unknown wire type. */
        private fun fields(from: Int = 0, to: Int = bytes.size): List<Field> {
            val out = ArrayList<Field>()
            var i = from
            fun varint(): Long {
                var result = 0L
                var shift = 0
                while (i < to) {
                    val b = bytes[i++].toInt()
                    result = result or ((b and 0x7f).toLong() shl shift)
                    if (b and 0x80 == 0) break
                    shift += 7
                }
                return result
            }
            fun fixed(size: Int): Long {
                var result = 0L
                for (k in 0 until size) {
                    result = result or ((bytes[i + k].toLong() and 0xff) shl (8 * k))
                }
                i += size
                return result
            }
            while (i < to) {
                val key = varint()
                val number = (key ushr 3).toInt()
                when ((key and 7).toInt()) {
                    0 -> out += Field(number, varint(), 0, 0)

                    1 -> out += Field(number, fixed(8), 0, 0)

                    2 -> {
                        val size = varint().toInt()
                        out += Field(number, size.toLong(), i, minOf(i + size, to))
                        i += size
                    }

                    5 -> out += Field(number, fixed(4), 0, 0)

                    else -> return out
                }
            }
            return out
        }

        private fun List<Field>.number(n: Int) = firstOrNull { it.number == n }?.value ?: 0L
        private fun List<Field>.text(n: Int) =
            firstOrNull { it.number == n }?.let { String(bytes, it.start, it.end - it.start) }
                ?: ""
        private fun Field.message() = fields(start, end)

        override fun toString(): String = buildString {
            val top = fields()
            val crashing = top.number(6)
            top.firstOrNull { it.number == 10 }?.message()?.let { signal ->
                appendLine(
                    "signal: ${signal.text(2)} (${signal.text(4)}) fault address " +
                        "0x%x".format(signal.number(9))
                )
            }
            top.text(14).takeIf { it.isNotEmpty() }?.let { appendLine("abort: $it") }
            // threads: map<uint32, Thread>, each entry {1: key, 2: Thread}.
            val threads = top.filter { it.number == 16 }
                .mapNotNull { entry -> entry.message().firstOrNull { it.number == 2 }?.message() }
            appendLine("threads: " + threads.joinToString { "${it.number(1)} ${it.text(2)}" })
            threads.firstOrNull { it.number(1) == crashing }?.let { thread ->
                appendLine("crashing thread ${thread.number(1)} \"${thread.text(2)}\":")
                thread.filter { it.number == 4 }.forEachIndexed { n, entry ->
                    val frame = entry.message()
                    appendLine(
                        "  #%02d rel_pc 0x%x %s (%s+%d) BuildId %s".format(
                            n,
                            frame.number(1),
                            frame.text(6),
                            frame.text(4),
                            frame.number(5),
                            frame.text(8)
                        )
                    )
                }
            }
            // log_buffers: {1: name, 2: LogMessage {1: time, 4: priority, 5: tag, 6: text}}.
            top.filter { it.number == 18 }.map { it.message() }.forEach { buffer ->
                appendLine("log ${buffer.text(1)}:")
                buffer.filter { it.number == 2 }.takeLast(80).forEach { entry ->
                    val line = entry.message()
                    appendLine("  ${line.text(1)} ${line.text(5)}: ${line.text(6).trimEnd()}")
                }
            }
        }
    }
}
