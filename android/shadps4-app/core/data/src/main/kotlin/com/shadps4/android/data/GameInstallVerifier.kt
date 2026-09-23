package com.shadps4.android.data

import java.io.File
import com.shadps4.android.runtime.session.NativeFexSession

object GameInstallVerifier {
    val REQUIRED_FILES: List<String> = listOf("eboot.bin", "sce_sys/param.sfo")

    sealed class VerifyResult {
        data class Ok(val bytesTotal: Long) : VerifyResult()
        data class Fail(val code: InstallErrorCode, val message: String) : VerifyResult()
    }

    /**
     * Entry the runtime opens: an archive linked from a user folder, an archive copied
     * into the game directory, or a plain `eboot.bin` tree.
     */
    fun executableFile(gameDir: File): File {
        ArchiveLinkIo.target(gameDir)?.let { return it }
        val archive = File(gameDir, "${gameDir.name}.zar")
        return if (archive.isFile) archive else File(gameDir, "eboot.bin")
    }

    /** True when [executable] is inside [root] or is the archive [root] links to. */
    fun executableAdmitted(root: File, executable: File): Boolean {
        if (executable.canonicalFile.toPath().startsWith(root.canonicalFile.toPath())) return true
        val linked = ArchiveLinkIo.target(root)?.canonicalFile ?: return false
        return linked == executable.canonicalFile
    }

    fun requiredFilesPresent(gameDir: File): Boolean {
        val eboot = executableFile(gameDir)
        val sfo = File(gameDir, "sce_sys/param.sfo")
        return eboot.isFile && eboot.length() > 0L && sfo.isFile && sfo.length() > 0L
    }

    fun canLaunch(filesDir: File, relativePath: String): Boolean {
        val gamesRoot = File(filesDir, "games").canonicalFile
        val root = File(filesDir, relativePath).canonicalFile
        if (!root.toPath().startsWith(gamesRoot.toPath())) return false
        if (!root.isDirectory) return false
        val manifest = InstallManifestIo.read(root) ?: return false
        if (manifest.status != InstallManifestIo.STATUS_INSTALLED) return false
        val eboot = executableFile(root).canonicalFile
        return executableAdmitted(root, eboot) && eboot.isFile && eboot.length() > 0L
    }

    fun verifyTreeForRegistration(
        gameDir: File,
        expectedGameId: String?,
        inspectArchive: (File) -> Array<ByteArray>? = { NativeFexSession.nativeInspectArchive(it.absolutePath) },
    ): VerifyResult {
        if (!gameDir.isDirectory) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "Game directory missing")
        }
        val eboot = executableFile(gameDir)
        if (eboot.extension == "zar") {
            val root = gameDir.canonicalFile.toPath()
            if (gameDir.listFiles().orEmpty().any { !it.canonicalFile.toPath().startsWith(root) }) {
                return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "archive install escapes game directory")
            }
            val metadata = runCatching { inspectArchive(eboot) }.getOrNull()
            if (metadata == null || metadata.size != 2 || metadata[0].isEmpty()) {
                return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "invalid game or update archive")
            }
            val id = runCatching { ParamSfoReader.parse(metadata[0]).titleId }.getOrNull()
            if (id == null || id != gameDir.name || (expectedGameId != null && id != expectedGameId)) {
                return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "archive title id mismatch")
            }
            if (!executableAdmitted(gameDir, eboot)) {
                return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "archive escapes game directory")
            }
            // Only UI metadata is cached. eboot and all game data remain packed.
            val sceSys = File(gameDir, "sce_sys")
            if (!sceSys.canonicalFile.toPath().startsWith(root)) {
                return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "metadata cache escapes game directory")
            }
            sceSys.mkdirs()
            for ((index, name) in listOf("param.sfo", "icon0.png").withIndex()) {
                val cache = File(sceSys, name)
                if (!cache.canonicalFile.toPath().startsWith(root)) {
                    return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "metadata cache escapes game directory")
                }
                if (metadata[index].isNotEmpty() && (!cache.isFile || !cache.readBytes().contentEquals(metadata[index]))) {
                    cache.writeBytes(metadata[index])
                }
            }
        }
        if (!eboot.isFile || eboot.length() <= 0L) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "missing or empty eboot.bin")
        }
        val sfoFile = File(gameDir, "sce_sys/param.sfo")
        if (!sfoFile.isFile || sfoFile.length() <= 0L) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "missing sce_sys/param.sfo")
        }
        if (expectedGameId != null) {
            val meta = runCatching { ParamSfoReader.parse(sfoFile.readBytes()) }.getOrNull()
            val titleId = meta?.titleId
            if (!titleId.isNullOrBlank() &&
                !titleId.equals(expectedGameId, ignoreCase = true) &&
                !expectedGameId.startsWith(titleId, ignoreCase = true)
            ) {
                // Folder imports often use folder name as id; only hard-fail on clear mismatch
                // when both look like TITLE_ID form and differ.
                if (TITLE_ID.matches(expectedGameId) && TITLE_ID.matches(titleId) &&
                    !expectedGameId.equals(titleId, ignoreCase = true)
                ) {
                    return VerifyResult.Fail(
                        InstallErrorCode.VERIFY_FAILED,
                        "title id mismatch: expected $expectedGameId got $titleId",
                    )
                }
            }
        }
        var total = 0L
        gameDir.walkTopDown().forEach { file ->
            if (file.isFile) total += file.length().coerceAtLeast(0L)
        }
        if (total <= 0L) {
            return VerifyResult.Fail(InstallErrorCode.VERIFY_FAILED, "empty game tree")
        }
        return VerifyResult.Ok(total)
    }

    private val TITLE_ID = Regex("^[A-Za-z]{4}[0-9]{5}$")
}
