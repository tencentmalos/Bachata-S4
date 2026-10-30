package com.shadps4.android.data

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.StandardCopyOption
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest

data class SaveFile(val path: String, val bytes: Long, val modifiedMs: Long)
data class SaveEntry(
    val userId: String,
    val titleId: String,
    val directory: String,
    val path: String,
    val title: String?,
    val subtitle: String?,
    val detail: String?,
    val bytes: Long,
    val modifiedMs: Long,
    val files: List<SaveFile>,
    val note: String,
    val warnings: List<String>,
)

/** Read-only view of real saves. Notes live outside the payload and never change save timestamps. */
class SaveLibrary(home: File, private val notes: File) {
    private val home = home.canonicalFile.toPath()

    fun list(titleId: String): List<SaveEntry> {
        require(titleId.matches(Regex("[A-Za-z0-9_-]{1,32}"))) { "Invalid save title ID" }
        if (!Files.exists(home)) return emptyList()
        require(Files.isDirectory(home)) { "Save home is not a directory: $home" }
        return Files.newDirectoryStream(home).use { users ->
            users.filter { it.fileName.toString().matches(Regex("[0-9]+")) &&
                Files.isDirectory(it, NOFOLLOW_LINKS) }.flatMap { user ->
                val savedata = user.resolve("savedata")
                val title = savedata.resolve(titleId)
                if (!Files.isDirectory(savedata, NOFOLLOW_LINKS) ||
                    !Files.isDirectory(title, NOFOLLOW_LINKS)) emptyList()
                else Files.newDirectoryStream(title).use { dirs ->
                    dirs.filter { Files.isDirectory(it, NOFOLLOW_LINKS) }
                        .map { scan(user.fileName.toString(), titleId, it) }.toList()
                }
            }.sortedWith(compareByDescending<SaveEntry> { it.modifiedMs }.thenBy { it.directory })
        }
    }

    private fun scan(user: String, title: String, dir: Path): SaveEntry {
        val files = mutableListOf<SaveFile>()
        val warnings = mutableListOf<String>()
        var size = 0L
        var modified = Files.getLastModifiedTime(dir, NOFOLLOW_LINKS).toMillis()
        Files.walkFileTree(dir, emptySet(), 32, object : SimpleFileVisitor<Path>() {
            override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                val name = dir.relativize(file).toString()
                if (attrs.isRegularFile) {
                    size += attrs.size()
                    modified = maxOf(modified, attrs.lastModifiedTime().toMillis())
                    if (files.size < 10000) files += SaveFile(name, attrs.size(), attrs.lastModifiedTime().toMillis())
                } else warnings += "Skipped link or nested directory: $name"
                return FileVisitResult.CONTINUE
            }
            override fun visitFileFailed(file: Path, error: IOException): FileVisitResult {
                warnings += "Cannot read ${dir.relativize(file)}: ${error.message}"
                return FileVisitResult.CONTINUE
            }
        })
        if (files.size == 10000) warnings += "File list limited to 10,000 entries"
        val sys = dir.resolve("sce_sys")
        val sfo = sys.resolve("param.sfo")
        val metadata = if (Files.isDirectory(sys, NOFOLLOW_LINKS) &&
            Files.isRegularFile(sfo, NOFOLLOW_LINKS) && Files.size(sfo) <= 1024 * 1024) {
            ParamSfoReader.parse(Files.readAllBytes(sfo))
        } else ParamSfoMetadata(null, null)
        val note = noteFile(user, title, dir.fileName.toString()).let {
            if (!it.exists()) "" else {
                require(it.length() <= MAX_NOTE_BYTES) { "Note is too large" }
                it.readText(Charsets.UTF_8)
            }
        }
        return SaveEntry(user, title, dir.fileName.toString(), dir.toString(), metadata.title,
            metadata.subtitle, metadata.detail, size, modified, files.sortedBy { it.path }, note, warnings)
    }

    fun writeNote(entry: SaveEntry, text: String) {
        require(text.length <= MAX_NOTE_CHARS && text.toByteArray(Charsets.UTF_8).size <= MAX_NOTE_BYTES) {
            "Note is too long"
        }
        require(entry.userId.matches(Regex("[0-9]+")) &&
            entry.titleId.matches(Regex("[A-Za-z0-9_-]{1,32}")) &&
            entry.directory.isNotEmpty() && entry.directory !in listOf(".", "..") &&
            '/' !in entry.directory && '\\' !in entry.directory) { "Invalid save identity" }
        val path = home.resolve(entry.userId).resolve("savedata").resolve(entry.titleId).resolve(entry.directory)
        require(path.toString() == entry.path && Files.isDirectory(path, NOFOLLOW_LINKS) &&
            path.toFile().canonicalFile.toPath() == path) { "Save was moved or removed; refresh the list" }
        val destination = noteFile(entry.userId, entry.titleId, entry.directory)
        Files.createDirectories(notes.toPath())
        if (text.isEmpty()) {
            Files.deleteIfExists(destination.toPath())
            return
        }
        val temporary = Files.createTempFile(notes.toPath(), ".note-", ".tmp")
        try {
            FileOutputStream(temporary.toFile()).use {
                it.write(text.toByteArray(Charsets.UTF_8))
                it.fd.sync()
            }
            // A failed atomic replacement leaves the previous note intact.
            Files.move(temporary, destination.toPath(), StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING)
        } finally { Files.deleteIfExists(temporary) }
    }

    private fun noteFile(user: String, title: String, directory: String): File {
        val identity = listOf(home.toString(), user, title, directory).joinToString("\u0000")
        val hash = MessageDigest.getInstance("SHA-256").digest(identity.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        return File(notes, "$hash.txt")
    }

    companion object {
        const val MAX_NOTE_CHARS = 4096
        private const val MAX_NOTE_BYTES = MAX_NOTE_CHARS * 4L
    }
}
