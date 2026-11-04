// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

package org.citra.citra_emu.utils

import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.io.BufferedInputStream
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.R
import org.ini4j.Wini

/**
 * Claudhar: the GPU driver each game uses. Azahar loads one custom driver, the one picked in
 * the GPU Driver Manager, for the whole app. A game can instead use another driver from the
 * gpu_drivers folder or the system driver. The choice is kept in config/drivers.ini in the
 * user folder, one section per title ID, next to the per-application settings, and applies
 * when the game next starts: [prepare] loads the driver just before the game boots. A chosen
 * driver is unpacked once into a private folder named after the SHA-256 of its zip, so the
 * Driver Manager's installation is never touched and a zip replaced under the same name
 * cannot change an unpacked driver. Adapted from the owner's Uberhar build.
 */
object GameDrivers {
    /** The choice for the system driver; any other is the file name of a zip in gpu_drivers. */
    const val SYSTEM = "system"

    private const val FILE_NAME = "drivers.ini"
    private const val UNPACKED_DIR = "gpu_driver_games"
    private const val KEPT_UNPACKED = 8
    private const val MAX_ZIP_BYTES = 128L * 1024 * 1024

    /** A game's driver: [archive] is [SYSTEM] or a zip in gpu_drivers, [name] what is shown. */
    data class Choice(val archive: String, val name: String)

    /** The driver chosen for [gameId], or null when it uses the global one. Any thread. */
    fun load(gameId: String): Choice? {
        val ini = read()
        val archive = ini.get(gameId, "archive")?.takeIf { it.isNotBlank() } ?: return null
        return Choice(archive, ini.get(gameId, "name") ?: archive)
    }

    /** Saves [choice] for [gameId]; null returns the game to the global driver. */
    fun save(gameId: String, choice: Choice?): Boolean {
        val ini = read()
        if (choice == null) {
            ini.remove(gameId)
        } else {
            ini.put(gameId, "archive", choice.archive)
            ini.put(gameId, "name", choice.name)
        }
        return write(ini)
    }

    /** Forgets every game's choice, as resetting all settings does. */
    fun clear() {
        runCatching { configDirectory()?.findFile(FILE_NAME)?.delete() }
    }

    /** What the settings screen shows for [gameId]'s driver. Main thread. */
    fun label(context: Context, gameId: String): String = load(gameId)?.name
        ?: context.getString(R.string.game_gpu_driver_global, globalName(context))

    /**
     * Lets the user pick [gameId]'s driver: the global one, the system driver or a zip in
     * gpu_drivers. Saves the pick and calls [changed]. Main thread.
     */
    fun showPicker(activity: AppCompatActivity, gameId: String, changed: () -> Unit) {
        activity.lifecycleScope.launch {
            // Reading each zip's metadata goes through the user folder; keep it off the UI.
            val choices = withContext(Dispatchers.IO) {
                runCatching { GpuDriverHelper.getDrivers() }.getOrDefault(mutableListOf())
                    .map { (uri, metadata) ->
                        // getDrivers lists the system driver with an empty Uri.
                        if (uri == Uri.EMPTY) {
                            Choice(SYSTEM, activity.getString(R.string.system_gpu_driver))
                        } else {
                            val file = FileUtil.getFilename(uri)
                            Choice(file, metadata.name ?: file)
                        }
                    }
            }
            val labels = listOf(
                activity.getString(R.string.game_gpu_driver_global, globalName(activity))
            ) + choices.map { it.name }
            val saved = load(gameId)
            val selected = if (saved == null) {
                0
            } else {
                val index = choices.indexOfFirst { it.archive == saved.archive }
                if (index < 0) -1 else index + 1
            }
            MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.game_gpu_driver)
                .setSingleChoiceItems(labels.toTypedArray(), selected) { dialog, index ->
                    if (!save(gameId, if (index == 0) null else choices[index - 1])) {
                        Toast.makeText(
                            activity,
                            R.string.game_gpu_driver_save_failed,
                            Toast.LENGTH_LONG
                        ).show()
                    }
                    changed()
                    dialog.dismiss()
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    /**
     * Loads the driver chosen for [gameId], or the global driver for a game without a choice
     * or whose driver cannot be loaded. Called on the emulation thread just before the game
     * boots; does nothing when the right driver is loaded already, so games that use the global
     * driver start as before. A game still closing, as when a shortcut starts another, is
     * waited for, as the boot waits for it anyway.
     */
    @Synchronized
    fun prepare(gameId: String?) {
        val choice = gameId?.let { load(it) }
        val outcome = when {
            choice == null -> GpuDriverHelper.loadGlobalDriver(waitForGame = true)
            choice.archive == SYSTEM -> GpuDriverHelper.loadDriver(null, null, waitForGame = true)
            else -> try {
                val (directory, library) = unpack(choice.archive)
                GpuDriverHelper.loadDriver(directory, library, waitForGame = true)
            } catch (e: Exception) {
                Log.warning("[GameDrivers] ${choice.name} for $gameId not usable: ${e.message}")
                0
            }
        }
        // A custom driver that did not load gives the system driver (1) or nothing (0).
        val failed = choice != null && choice.archive != SYSTEM && outcome != 2
        if (failed) {
            val fallback = GpuDriverHelper.loadGlobalDriver(waitForGame = true)
            Handler(Looper.getMainLooper()).post {
                val context = CitraApplication.appContext
                Toast.makeText(
                    context,
                    context.getString(R.string.game_gpu_driver_failed, globalName(context)),
                    Toast.LENGTH_LONG
                ).show()
            }
            Log.warning("[GameDrivers] $gameId: chosen driver failed, global driver gave $fallback")
        }
        Log.info(
            "[GameDrivers] $gameId: driver ${choice?.name ?: "global"}, " +
                "loaded ${if (outcome == 2) "custom" else if (outcome == 1) "system" else "none"}"
        )
    }

    /** The global driver's name, as the Driver Manager shows it. */
    private fun globalName(context: Context): String =
        GpuDriverHelper.customDriverData.name ?: context.getString(R.string.system_gpu_driver)

    /**
     * Unpacks the zip [archive] from gpu_drivers into its own folder, once, and returns that
     * folder and the driver library's file name. The zip is hashed from a private copy, so the
     * files unpacked are exactly the ones hashed.
     */
    private fun unpack(archive: String): Pair<String, String> {
        val context = CitraApplication.appContext
        val zip = GpuDriverHelper.driverStoragePath.findFile(archive)?.takeIf { it.isFile }
            ?: throw IOException("$archive is no longer in gpu_drivers")
        val root = File(context.filesDir, UNPACKED_DIR)
        if (!root.isDirectory && !root.mkdirs()) throw IOException("cannot create $root")
        val copy = File.createTempFile("driver-", ".zip", context.cacheDir)
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            var total = 0L
            context.contentResolver.openInputStream(zip.uri)!!.use { input ->
                copy.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    var count = input.read(buffer)
                    while (count >= 0) {
                        total += count
                        if (total > MAX_ZIP_BYTES) throw IOException("$archive is over 128 MiB")
                        digest.update(buffer, 0, count)
                        output.write(buffer, 0, count)
                        count = input.read(buffer)
                    }
                }
            }
            val folder = File(root, digest.digest().joinToString("") { "%02x".format(it) })
            if (!folder.isDirectory) {
                val staging = File(root, "${folder.name}.partial")
                staging.deleteRecursively()
                try {
                    val stream = BufferedInputStream(copy.inputStream())
                    FileUtil.unzipToInternalStorage(stream, staging)
                    if (!staging.renameTo(folder)) throw IOException("cannot unpack $archive")
                } finally {
                    staging.deleteRecursively()
                }
            }
            val metadata = GpuDriverMetadata(File(folder, "meta.json"))
            if (metadata.minApi > Build.VERSION.SDK_INT) {
                throw IOException("$archive needs Android API ${metadata.minApi}")
            }
            val library = metadata.libraryName
                ?.takeIf { it.isNotEmpty() && File(it).name == it && File(folder, it).isFile }
                ?: throw IOException("$archive has no driver library")
            folder.setLastModified(System.currentTimeMillis())
            prune(root, folder)
            return Pair(folder.canonicalPath + "/", library)
        } finally {
            copy.delete()
        }
    }

    /** Keeps the [KEPT_UNPACKED] most recently used unpacked drivers, [current] among them. */
    private fun prune(root: File, current: File) {
        root.listFiles()
            ?.filter { it.isDirectory && it != current && !it.name.endsWith(".partial") }
            ?.sortedByDescending { it.lastModified() }
            ?.drop(KEPT_UNPACKED - 1)
            ?.forEach { it.deleteRecursively() }
    }

    private fun configDirectory(): DocumentFile? =
        DocumentFile.fromTreeUri(CitraApplication.appContext, PermissionsHandler.citraDirectory)
            ?.findFile("config")

    /** The driver choices; empty when there are none yet or the file cannot be read. */
    private fun read(): Wini = try {
        configDirectory()?.findFile(FILE_NAME)?.let { file ->
            CitraApplication.appContext.contentResolver.openInputStream(file.uri)?.use { Wini(it) }
        } ?: Wini()
    } catch (e: Exception) {
        Log.error("[GameDrivers] Cannot read $FILE_NAME: ${e.message}")
        Wini()
    }

    private fun write(ini: Wini): Boolean = try {
        val directory = configDirectory() ?: throw IOException("no config folder")
        val file = directory.findFile(FILE_NAME) ?: directory.createFile("*/*", FILE_NAME)
            ?: throw IOException("cannot create $FILE_NAME")
        CitraApplication.appContext.contentResolver.openOutputStream(file.uri, "wt")!!.use {
            ini.store(it)
        }
        true
    } catch (e: Exception) {
        Log.error("[GameDrivers] Cannot save $FILE_NAME: ${e.message}")
        false
    }
}
