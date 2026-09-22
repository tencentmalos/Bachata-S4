package com.shadps4.android.data

import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File

/**
 * Registers `.zar` archives that live in a user folder ([ZarLibraryFolder]) without
 * copying them into app storage.
 *
 * Each archive gets an app-private directory `games/<TITLE_ID>/` holding only a link
 * to the archive plus the cached `sce_sys` metadata the library UI needs. Everything
 * else — the game itself, its update and its DLC — stays in the user's folder, and the
 * runtime opens the archive by its real path, so the host finds the `-UPD` / `-DLC`
 * siblings next to it exactly as for an app-private install.
 *
 * Overlay archives are not candidates on their own: they are reached through their base.
 */
object ExternalArchiveLibrary {
    const val MODE = "external-archive"

    /** Suffixes the host resolves relative to a base archive (Core::FileSys). */
    val OVERLAY_SUFFIXES: List<String> = listOf("-UPDATE", "-UPD", "-patch", "-mods", "-DLC")

    data class LinkedGame(val id: String, val archive: File, val directory: File)

    data class SyncResult(
        val linked: List<LinkedGame> = emptyList(),
        val removed: List<String> = emptyList(),
        val failures: Map<String, String> = emptyMap(),
    )

    /** True when [name] (a file name or stem) is an overlay of some base archive. */
    fun isOverlayName(name: String): Boolean {
        val stem = name.removeSuffix(".zar")
        return OVERLAY_SUFFIXES.any { stem.endsWith(it, ignoreCase = true) }
    }

    /** Base archives directly inside [folder], sorted by name; overlays are skipped. */
    fun baseArchives(folder: File?): List<File> {
        if (folder == null) return emptyList()
        val entries = runCatching { folder.listFiles() }.getOrNull() ?: return emptyList()
        return entries
            .filter { it.isFile && it.extension.equals("zar", ignoreCase = true) && it.length() > 0L }
            .filterNot { isOverlayName(it.name) }
            .sortedBy { it.name.lowercase() }
    }

    /**
     * Brings `games/` in line with [folder]: links new archives, refreshes links whose
     * target changed, and removes links whose archive is gone or no longer configured.
     * Installed (copied) games are never touched.
     */
    fun sync(
        filesDir: File,
        folder: File?,
        inspectArchive: (File) -> Array<ByteArray>? = {
            NativeFexSession.nativeInspectArchive(it.absolutePath)
        },
        nowMs: () -> Long = System::currentTimeMillis,
    ): SyncResult {
        val gamesRoot = File(filesDir, "games").canonicalFile
        val linked = ArrayList<LinkedGame>()
        val removed = ArrayList<String>()
        val failures = LinkedHashMap<String, String>()
        val keep = HashSet<String>()

        for (archive in baseArchives(folder)) {
            val canonical = runCatching { archive.canonicalFile }.getOrNull() ?: archive
            val existing = findLinkFor(gamesRoot, canonical)
            if (existing != null && ArchiveLinkIo.matches(existing.first, canonical)) {
                keep += existing.second
                linked += LinkedGame(existing.second, canonical, existing.first)
                continue
            }
            val metadata = runCatching { inspectArchive(canonical) }.getOrNull()
            if (metadata == null || metadata.size != 2 || metadata[0].isEmpty()) {
                failures[canonical.name] = "invalid game archive"
                continue
            }
            val id = runCatching { ParamSfoReader.parse(metadata[0]).titleId }.getOrNull()
                ?.trim()
                ?.takeIf { it.isNotEmpty() && SAFE_ID.matches(it) }
            if (id == null) {
                failures[canonical.name] = "archive title id missing"
                continue
            }
            val directory = File(gamesRoot, id)
            val link = ArchiveLinkIo.read(directory)
            if (directory.isDirectory && link == null) {
                // An installed copy of the same title wins; never replace it with a link.
                failures[canonical.name] = "title $id is already installed"
                continue
            }
            if (!directory.isDirectory && !directory.mkdirs()) {
                failures[canonical.name] = "cannot create library entry"
                continue
            }
            // The link is written first so verification resolves the same executable the
            // runtime will open; a rejected archive leaves no half-registered entry.
            ArchiveLinkIo.write(
                directory,
                ArchiveLink(canonical.absolutePath, canonical.length(), canonical.lastModified()),
            )
            val verify = GameInstallVerifier.verifyTreeForRegistration(
                directory,
                id,
                inspectArchive = { metadata },
            )
            if (verify is GameInstallVerifier.VerifyResult.Fail) {
                failures[canonical.name] = verify.message
                directory.deleteRecursively()
                continue
            }
            InstallManifestIo.write(
                directory,
                InstallManifest(
                    status = InstallManifestIo.STATUS_INSTALLED,
                    gameId = id,
                    contentId = null,
                    mode = MODE,
                    sourceUri = "file://${canonical.absolutePath}",
                    installedAtMs = InstallManifestIo.read(directory)?.installedAtMs ?: nowMs(),
                    requiredFiles = GameInstallVerifier.REQUIRED_FILES,
                    bytesTotal = canonical.length(),
                ),
            )
            keep += id
            linked += LinkedGame(id, canonical, directory)
        }

        // Links that no longer point at a configured, existing archive are dropped.
        for (directory in gamesRoot.listFiles().orEmpty()) {
            if (!directory.isDirectory || directory.name in keep) continue
            val link = ArchiveLinkIo.read(directory) ?: continue
            val target = File(link.path)
            val stillListed = folder != null &&
                runCatching { target.canonicalFile.parentFile == folder.canonicalFile }.getOrDefault(false)
            if (!stillListed || !target.isFile) {
                directory.deleteRecursively()
                removed += directory.name
            }
        }
        return SyncResult(linked, removed, failures)
    }

    private fun findLinkFor(gamesRoot: File, archive: File): Pair<File, String>? {
        for (directory in gamesRoot.listFiles().orEmpty()) {
            if (!directory.isDirectory) continue
            val link = ArchiveLinkIo.read(directory) ?: continue
            if (link.path == archive.absolutePath) return directory to directory.name
        }
        return null
    }

    private val SAFE_ID = Regex("^[A-Za-z0-9._-]+$")
}

/** Where an external archive lives, with the identity the link was validated against. */
data class ArchiveLink(val path: String, val bytes: Long, val modifiedAtMs: Long)

object ArchiveLinkIo {
    const val FILE_NAME = "archive.link"

    fun write(dir: File, link: ArchiveLink) {
        dir.mkdirs()
        File(dir, FILE_NAME).writeText(
            "path=${link.path}\nbytes=${link.bytes}\nmodifiedAtMs=${link.modifiedAtMs}\n",
        )
    }

    fun read(dir: File): ArchiveLink? {
        val file = File(dir, FILE_NAME)
        if (!file.isFile) return null
        val map = runCatching { file.readText() }.getOrNull()?.lineSequence()?.mapNotNull { line ->
            val idx = line.indexOf('=')
            if (idx <= 0) null else line.substring(0, idx) to line.substring(idx + 1)
        }?.toMap() ?: return null
        val path = map["path"]?.trim()?.takeIf { it.isNotEmpty() && File(it).isAbsolute } ?: return null
        return ArchiveLink(
            path = path,
            bytes = map["bytes"]?.trim()?.toLongOrNull() ?: 0L,
            modifiedAtMs = map["modifiedAtMs"]?.trim()?.toLongOrNull() ?: 0L,
        )
    }

    /** Resolved archive of [dir], or null when the link is absent or its target is gone. */
    fun target(dir: File): File? {
        val link = read(dir) ?: return null
        val file = File(link.path)
        return file.takeIf { it.isFile && it.length() > 0L }
    }

    /** True when [dir] already links to [archive] with the size and time it was read at. */
    fun matches(dir: File, archive: File): Boolean {
        val link = read(dir) ?: return false
        if (link.path != archive.absolutePath) return false
        if (!archive.isFile) return false
        if (link.bytes != archive.length() || link.modifiedAtMs != archive.lastModified()) return false
        return File(dir, "sce_sys/param.sfo").isFile &&
            InstallManifestIo.read(dir)?.status == InstallManifestIo.STATUS_INSTALLED
    }
}
