package com.shadps4.android.runtime.session

import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileNotFoundException
import java.security.MessageDigest

/**
 * Installs the packaged PS4 firmware LLE modules (assets/ps4-firmware, pinned by
 * runtime/locks/ps4-firmware-modules.json) into the host's default sys_modules
 * directory. A file the user placed there themselves is never replaced; a file a
 * previous bundle installed is upgraded, and removed when no longer bundled.
 */
object FirmwareModules {
    private const val TAG = "FirmwareModules"
    private const val ASSETS = "ps4-firmware"
    private const val RECORD = ".bundled-firmware.json"

    data class Result(val installed: Int, val current: Int, val userOwned: List<String>, val removed: Int)

    @Synchronized fun prepare(context: Context): Result {
        val index = try {
            context.assets.open("$ASSETS/index.json").bufferedReader().use { JSONObject(it.readText()) }
        } catch (e: FileNotFoundException) {
            Log.w(TAG, "APK has no firmware modules")
            return Result(0, 0, emptyList(), 0)
        }
        val directory = File(context.filesDir, "host/sys_modules").apply { mkdirs() }
        val recordFile = File(directory, RECORD)
        // file -> SHA-256 of every file a bundle wrote here, across APK updates.
        val previous = mutableMapOf<String, String>()
        if (recordFile.isFile) {
            val record = JSONObject(recordFile.readText())
            for (key in record.keys()) previous[key] = record.getString(key)
        }
        val bundled = mutableMapOf<String, String>()
        val modules = index.getJSONArray("modules")
        for (i in 0 until modules.length()) {
            val module = modules.getJSONObject(i)
            val name = module.getString("file")
            val sha = module.getString("sha256")
            check(File(name).name == name && sha.matches(Regex("[0-9a-f]{64}"))) { "Invalid firmware index" }
            bundled[name] = sha
        }
        var installed = 0
        var current = 0
        val userOwned = mutableListOf<String>()
        val owned = mutableMapOf<String, String>()
        for ((name, sha) in bundled) {
            val target = File(directory, name)
            val existing = if (target.isFile) sha(target) else null
            when {
                existing == sha -> { current++; owned[name] = sha }
                existing == null || existing == previous[name] -> {
                    install(context, name, sha, directory, target)
                    installed++
                    owned[name] = sha
                }
                else -> userOwned += name
            }
        }
        var removed = 0
        for ((name, sha) in previous) {
            if (name in bundled) continue
            val target = File(directory, name)
            if (target.isFile && sha(target) == sha && target.delete()) removed++
        }
        writeRecord(recordFile, owned)
        if (userOwned.isNotEmpty()) Log.i(TAG, "Keeping user-provided modules: $userOwned")
        Log.i(TAG, "Firmware ${index.optString("firmware")} modules: installed=$installed current=$current " +
            "user=${userOwned.size} removed=$removed")
        return Result(installed, current, userOwned, removed)
    }

    private fun install(context: Context, name: String, sha: String, directory: File, target: File) {
        val temporary = File.createTempFile("firmware-", ".tmp", directory)
        try {
            context.assets.open("$ASSETS/$name").use { input ->
                temporary.outputStream().use { output -> input.copyTo(output) }
            }
            check(sha(temporary) == sha) { "Bundled firmware module hash mismatch: $name" }
            check(temporary.renameTo(target)) { "Cannot install firmware module $name" }
        } finally { temporary.delete() }
    }

    private fun writeRecord(file: File, owned: Map<String, String>) {
        val json = JSONObject()
        for ((name, sha) in owned.toSortedMap()) json.put(name, sha)
        val temporary = File(file.parentFile, "$RECORD.tmp")
        temporary.writeText(json.toString(1) + "\n")
        check(temporary.renameTo(file)) { "Cannot record bundled firmware modules" }
    }

    private fun sha(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(65536)
            while (true) {
                val size = input.read(buffer)
                if (size < 0) break
                digest.update(buffer, 0, size)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
    }
}
