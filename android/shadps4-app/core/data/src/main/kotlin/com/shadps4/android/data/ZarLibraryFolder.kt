package com.shadps4.android.data

import android.content.Context
import android.os.Environment
import java.io.File

/**
 * Folder outside app storage that holds `.zar` game archives.
 *
 * The archives stay where the user put them: the library links to them (see
 * [ExternalArchiveLibrary]) instead of copying tens of gigabytes into app storage,
 * and the runtime opens the archive by its real path so the host resolves the
 * `-UPD` / `-DLC` siblings that sit next to it.
 *
 * Stored as a plain absolute path in the app preference file, because both Kotlin
 * (`File`) and the native loader read it directly. A SAF tree URI cannot be opened
 * by path, so [fromTreeUri] converts a picked tree to its filesystem location and
 * refuses trees that have no such location.
 */
object ZarLibraryFolder {
    const val FILE_NAME = UiOrientationPreference.FILE_NAME
    const val KEY = "zar_library_folder"

    /** Readable without any storage permission; the default suggestion in Settings. */
    fun appExternalDefault(context: Context): File? =
        context.getExternalFilesDir(null)?.let { File(it, "roms") }

    /**
     * Trims, drops a `file://` prefix and requires an absolute device path. Resolved
     * lexically: the setting names a path on the device, which the host JVM must not
     * reinterpret, and a stored setting should not depend on what exists right now.
     * Returns null for blank input or anything that is not an absolute path.
     */
    fun normalize(raw: String?): String? {
        var value = raw?.trim().orEmpty()
        if (value.isEmpty()) return null
        if (value.startsWith("file://")) value = value.removePrefix("file://")
        if (!value.startsWith('/')) return null
        val parts = ArrayList<String>()
        for (part in value.split('/')) {
            when (part) {
                "", "." -> Unit
                ".." -> if (parts.isNotEmpty()) parts.removeAt(parts.lastIndex) else return null
                else -> parts.add(part)
            }
        }
        return "/" + parts.joinToString("/")
    }

    /**
     * Filesystem path of a SAF tree, or null when the tree is not a plain storage
     * folder (for example a cloud provider). `primary:game/ps4/roms` is the shared
     * storage root; other volume ids live under /storage/<id>.
     */
    fun fromTreeUri(
        uri: String?,
        primaryRoot: String = runCatching { Environment.getExternalStorageDirectory()?.absolutePath }
            .getOrNull() ?: "/sdcard",
    ): String? {
        val text = uri?.trim().orEmpty()
        if (!text.startsWith("content://com.android.externalstorage.documents/tree/")) return null
        val encoded = text.substringAfterLast('/')
        val documentId = runCatching { java.net.URLDecoder.decode(encoded, "UTF-8") }.getOrNull()
            ?: return null
        val volume = documentId.substringBefore(':', missingDelimiterValue = "")
        val relative = documentId.substringAfter(':', missingDelimiterValue = "")
        if (volume.isEmpty()) return null
        val root = if (volume == "primary") primaryRoot else "/storage/$volume"
        return normalize(if (relative.isEmpty()) root else "$root/$relative")
    }

    fun read(context: Context): String? =
        normalize(
            context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE).getString(KEY, null),
        )

    fun readFolder(context: Context): File? = read(context)?.let(::File)

    /** Writes a normalized path, or clears the setting when [path] is null or unusable. */
    fun write(context: Context, path: String?) {
        val prefs = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
        val normalized = normalize(path)
        prefs.edit().apply {
            if (normalized == null) remove(KEY) else putString(KEY, normalized)
        }.apply()
    }

    /** True when the folder exists and its entries can be listed by this process. */
    fun isReadable(folder: File?): Boolean {
        if (folder == null) return false
        return runCatching { folder.isDirectory && folder.listFiles() != null }.getOrDefault(false)
    }
}
