package com.shadps4.android.data

import com.shadps4.android.runtime.session.NativeFexSession
import com.shadps4.android.runtime.settings.DisplayMode
import com.shadps4.android.runtime.settings.ProfileScope
import java.io.File

object GameDisplayMode {
    suspend fun resolve(filesDir: File, profiles: RuntimeProfileStore, gameId: String,
                        relativePath: String): DisplayMode.Decision {
        require(GameInstallVerifier.canLaunch(filesDir, relativePath)) { "Game is not installed" }
        val root = File(filesDir, relativePath).canonicalFile
        val executable = GameInstallVerifier.executableFile(root).canonicalFile
        require(GameInstallVerifier.executableAdmitted(root, executable)) { "Invalid game path" }
        val bytes = requireNotNull(NativeFexSession.nativeReadLaunchParamSfo(executable.path)) {
            "Cannot read game display requirements"
        }
        val metadata = ParamSfoReader.parse(bytes)
        require(!metadata.titleId.isNullOrBlank()) { "Invalid game metadata" }
        return DisplayMode.resolve(profiles.load(ProfileScope.Global),
            profiles.load(ProfileScope.Game(gameId)), metadata.isPsvr)
    }
}
