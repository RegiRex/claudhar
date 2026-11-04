// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
// Modified by the Claudhar Project in 2026; changes are marked "Claudhar:".

package org.citra.citra_emu.utils

import android.net.Uri
import android.os.Build
import androidx.documentfile.provider.DocumentFile
import java.io.BufferedInputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.lang.IllegalStateException
import java.util.zip.ZipEntry
import java.util.zip.ZipException
import java.util.zip.ZipInputStream
import org.citra.citra_emu.CitraApplication
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.utils.FileUtil.asDocumentFile
import org.citra.citra_emu.utils.FileUtil.inputStream

object GpuDriverHelper {
    private const val META_JSON_FILENAME = "meta.json"
    private var fileRedirectionPath: String? = null
    var driverInstallationPath: String? = null
    private var hookLibPath: String? = null

    /**
     * Claudhar: the driver library loaded now, as folder and library name; a null name is the
     * system driver. Null before the first load.
     */
    private var loadedDriver: Pair<String?, String?>? = null

    val driverStoragePath: DocumentFile
        get() {
            // Bypass directory initialization checks
            val root = DocumentFile.fromTreeUri(
                CitraApplication.appContext,
                Uri.parse(DirectoryInitialization.userPath)
            )!!
            var driverDirectory = root.findFile("gpu_drivers")
            if (driverDirectory == null) {
                driverDirectory = FileUtil.createDir(root.uri.toString(), "gpu_drivers")
            }
            return driverDirectory!!
        }

    fun initializeDriverParameters() {
        try {
            // Initialize the file redirection directory.
            fileRedirectionPath =
                DirectoryInitialization.internalUserPath + "/gpu/vk_file_redirect/"

            // Initialize the driver installation directory.
            driverInstallationPath = CitraApplication.appContext
                .filesDir.canonicalPath + "/gpu_driver/"
        } catch (e: IOException) {
            throw RuntimeException(e)
        }

        // Initialize directories.
        initializeDirectories()

        // Initialize hook libraries directory.
        hookLibPath = CitraApplication.appContext.applicationInfo.nativeLibraryDir + "/"

        // Initialize GPU driver.
        // Claudhar: through loadGlobalDriver, which records what is loaded for GameDrivers.
        // Forced, since an install puts a new driver into the same folder.
        loadGlobalDriver(force = true)
    }

    /**
     * Claudhar: loads the driver library [libraryName] from [directory], or the system driver
     * when [libraryName] is null, unless it is loaded already and [force] is false. Returns 2
     * when the custom driver loaded, 1 for the system driver (also when the custom one failed)
     * and 0 when nothing changed. While a game is running, waits for it to end when
     * [waitForGame] is set, as for the next game's driver, and otherwise changes nothing.
     * Any thread, but only a thread that may wait should set [waitForGame].
     */
    @Synchronized
    fun loadDriver(
        directory: String?,
        libraryName: String?,
        force: Boolean = false,
        waitForGame: Boolean = false
    ): Int {
        val wanted = Pair(if (libraryName == null) null else directory, libraryName)
        if (!force && wanted == loadedDriver) {
            return if (libraryName == null) 1 else 2
        }
        val outcome = NativeLibrary.initializeGpuDriver(
            hookLibPath,
            directory ?: driverInstallationPath,
            libraryName,
            fileRedirectionPath,
            waitForGame
        )
        if (outcome != 0) {
            loadedDriver = if (outcome == 2) wanted else Pair(null, null)
        } else if (force) {
            // A forced load follows a driver install into the same folder. Refused while a game
            // runs, it leaves loaded a library whose files have since changed, so the next load
            // must not take it for the current one.
            loadedDriver = null
        }
        return outcome
    }

    /** Claudhar: loads the driver picked in the Driver Manager; see [loadDriver]. */
    fun loadGlobalDriver(force: Boolean = false, waitForGame: Boolean = false): Int =
        loadDriver(driverInstallationPath, customDriverData.libraryName, force, waitForGame)

    fun getDrivers(): MutableList<Pair<Uri, GpuDriverMetadata>> {
        val driverZips = driverStoragePath.listFiles()
        val drivers: MutableList<Pair<Uri, GpuDriverMetadata>> =
            driverZips
                .filter { it.isFile }
                .mapNotNull {
                    val metadata = getMetadataFromZip(it.inputStream())
                    metadata.name?.let { _ -> Pair(it.uri, metadata) }
                }
                .sortedByDescending { it: Pair<Uri, GpuDriverMetadata> -> it.second.name }
                .distinct()
                .toMutableList()

        // TODO: Get system driver information
        drivers.add(0, Pair(Uri.EMPTY, GpuDriverMetadata()))
        return drivers
    }

    fun installDefaultDriver() {
        // Removing the installed driver will result in the backend using the default system driver.
        File(driverInstallationPath!!).deleteRecursively()
        initializeDriverParameters()
    }

    fun copyDriverToExternalStorage(driverUri: Uri): DocumentFile? {
        // Ensure we have directories.
        initializeDirectories()

        // Copy the zip file URI to user data
        val copiedFile =
            FileUtil.copyToExternalStorage(driverUri, driverStoragePath) ?: return null

        // Validate driver
        val metadata = getMetadataFromZip(copiedFile.inputStream())
        if (metadata.name == null) {
            copiedFile.delete()
            return null
        }

        if (metadata.minApi > Build.VERSION.SDK_INT) {
            copiedFile.delete()
            return null
        }
        return copiedFile
    }

    /**
     * Copies driver zip into user data directory so that it can be exported along with
     * other user data and also unzipped into the installation directory
     */
    fun installCustomDriverComplete(driverUri: Uri): Boolean {
        // Revert to system default in the event the specified driver is bad.
        installDefaultDriver()

        // Ensure we have directories.
        initializeDirectories()

        // Copy the zip file URI to user data
        val copiedFile =
            FileUtil.copyToExternalStorage(driverUri, driverStoragePath) ?: return false

        // Validate driver
        val metadata = getMetadataFromZip(copiedFile.inputStream())
        if (metadata.name == null) {
            copiedFile.delete()
            return false
        }

        if (metadata.minApi > Build.VERSION.SDK_INT) {
            copiedFile.delete()
            return false
        }

        // Unzip the driver.
        try {
            FileUtil.unzipToInternalStorage(
                BufferedInputStream(copiedFile.inputStream()),
                File(driverInstallationPath!!)
            )
        } catch (e: SecurityException) {
            return false
        }

        // Initialize the driver parameters.
        initializeDriverParameters()

        return true
    }

    /**
     * Unzips driver into private installation directory
     */
    fun installCustomDriverPartial(driver: Uri): Boolean {
        // Revert to system default in the event the specified driver is bad.
        installDefaultDriver()

        // Ensure we have directories.
        initializeDirectories()

        // Validate driver
        val metadata = getMetadataFromZip(driver.inputStream())
        if (metadata.name == null) {
            driver.asDocumentFile()?.delete()
            return false
        }

        // Unzip the driver to the private installation directory
        try {
            FileUtil.unzipToInternalStorage(
                BufferedInputStream(driver.inputStream()),
                File(driverInstallationPath!!)
            )
        } catch (e: SecurityException) {
            return false
        }

        // Initialize the driver parameters.
        initializeDriverParameters()

        return true
    }

    /**
     * Takes in a zip file and reads the meta.json file for presentation to the UI
     *
     * @param driver Zip containing driver and meta.json file
     * @return A non-null [GpuDriverMetadata] instance that may have null members
     */
    fun getMetadataFromZip(driver: InputStream): GpuDriverMetadata {
        try {
            ZipInputStream(driver).use { zis ->
                var entry: ZipEntry? = zis.nextEntry
                while (entry != null) {
                    if (!entry.isDirectory && entry.name.lowercase().contains(".json")) {
                        val size = if (entry.size == -1L) 0L else entry.size
                        return GpuDriverMetadata(zis, size)
                    }
                    entry = zis.nextEntry
                }
            }
        } catch (_: ZipException) {
        }
        return GpuDriverMetadata()
    }

    external fun supportsCustomDriverLoading(): Boolean

    // Parse the custom driver metadata to retrieve the name.
    val customDriverData: GpuDriverMetadata
        get() = GpuDriverMetadata(File(driverInstallationPath + META_JSON_FILENAME))

    fun initializeDirectories() {
        // Ensure the file redirection directory exists.
        val fileRedirectionDir = File(fileRedirectionPath!!)
        if (!fileRedirectionDir.exists()) {
            fileRedirectionDir.mkdirs()
        }
        // Ensure the driver installation directory exists.
        val driverInstallationDir = File(driverInstallationPath!!)
        if (!driverInstallationDir.exists()) {
            driverInstallationDir.mkdirs()
        }
        // Ensure the driver storage directory exists
        if (!driverStoragePath.exists()) {
            throw IllegalStateException("Driver storage directory couldn't be created!")
        }
    }
}
