package com.shadps4.android.data

import com.shadps4.android.model.Game
import com.shadps4.android.runtime.session.NativeFexSession
import com.shadps4.android.runtime.settings.ProfileScope
import java.io.File
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull

data class GameSaveLibrary(val library: SaveLibrary, val titleId: String, val home: File) {
    companion object {
        suspend fun open(filesDir: File, profiles: RuntimeProfileStore, game: Game): GameSaveLibrary {
            val root = File(filesDir, game.relativePath).canonicalFile
            require(root.toPath().startsWith(filesDir.canonicalFile.toPath())) { "Invalid game path" }
            val sfo = File(root, "sce_sys/param.sfo")
            val bytes = if (EmuPspLibrary.managed(root) && sfo.isFile && sfo.length() <= 1024 * 1024) sfo.readBytes() else {
                val executable = GameInstallVerifier.executableFile(root).canonicalFile
                require(GameInstallVerifier.executableAdmitted(root, executable)) { "Invalid game path" }
                requireNotNull(NativeFexSession.nativeReadLaunchParamSfo(executable.path)) {
                    "Cannot read the game's save identity"
                }
            }
            val metadata = ParamSfoReader.parse(bytes)
            // The runtime uses INSTALL_DIR_SAVEDATA before TITLE_ID (shared regional saves).
            val title = requireNotNull(metadata.saveTitleId ?: metadata.titleId) { "Missing save title ID" }
            val profile = profiles.load(ProfileScope.Global)
            val configured = (profile.values["general.home_dir"] as? JsonPrimitive)?.contentOrNull.orEmpty()
            val home = if (configured.isBlank()) File(filesDir, "host/home") else File(configured).also {
                require(it.isAbsolute) { "Save home must be an absolute path: $configured" }
            }
            return GameSaveLibrary(SaveLibrary(home, File(filesDir, "save-notes")), title, home)
        }
    }
}
