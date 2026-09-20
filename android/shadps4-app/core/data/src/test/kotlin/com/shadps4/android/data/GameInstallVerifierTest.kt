package com.shadps4.android.data

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class GameInstallVerifierTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun canLaunchRequiresManifestAndEboot() {
        val filesDir = temporaryFolder.root
        val game = File(filesDir, "games/CUSA00000").apply { mkdirs() }
        assertFalse(GameInstallVerifier.canLaunch(filesDir, "games/CUSA00000"))
        File(game, "eboot.bin").writeBytes(byteArrayOf(1))
        assertFalse(GameInstallVerifier.canLaunch(filesDir, "games/CUSA00000"))
        InstallManifestIo.write(
            game,
            InstallManifest(
                status = InstallManifestIo.STATUS_INSTALLED,
                gameId = "CUSA00000",
                contentId = null,
                mode = "folder",
                sourceUri = "",
                installedAtMs = 1L,
                requiredFiles = listOf("eboot.bin", "sce_sys/param.sfo"),
                bytesTotal = 1L,
            ),
        )
        assertTrue(GameInstallVerifier.canLaunch(filesDir, "games/CUSA00000"))
    }

    @Test
    fun verifyTreeFailsWithoutEboot() {
        val game = temporaryFolder.newFolder("half")
        File(game, "sce_sys").mkdirs()
        File(game, "sce_sys/param.sfo").writeBytes(byteArrayOf(1))
        val result = GameInstallVerifier.verifyTreeForRegistration(game, null)
        assertTrue(result is GameInstallVerifier.VerifyResult.Fail)
        assertEquals(
            InstallErrorCode.VERIFY_FAILED,
            (result as GameInstallVerifier.VerifyResult.Fail).code,
        )
    }

    @Test
    fun verifyTreeOkWithEbootAndSfo() {
        val game = temporaryFolder.newFolder("full")
        File(game, "sce_sys").mkdirs()
        File(game, "sce_sys/param.sfo").writeBytes(byteArrayOf(1, 2, 3))
        File(game, "eboot.bin").writeBytes(byteArrayOf(9))
        val result = GameInstallVerifier.verifyTreeForRegistration(game, null)
        assertTrue(result is GameInstallVerifier.VerifyResult.Ok)
        assertEquals(4L, (result as GameInstallVerifier.VerifyResult.Ok).bytesTotal)
    }

    @Test
    fun archiveRegistrationCachesOnlyMetadataAndRejectsMismatchedUpdate() {
        val game = File(temporaryFolder.root, "games/CUSA00000").apply { mkdirs() }
        val archive = File(game, "CUSA00000.zar").apply { writeBytes(byteArrayOf(1, 2)) }
        val sfo = buildMinimalSfo(mapOf("TITLE_ID" to "CUSA00000", "APP_VER" to "01.08"))
        val result = GameInstallVerifier.verifyTreeForRegistration(game, "CUSA00000") {
            assertEquals(archive, it)
            arrayOf(sfo, byteArrayOf(9))
        }
        assertTrue(result is GameInstallVerifier.VerifyResult.Ok)
        assertEquals(archive, GameInstallVerifier.executableFile(game))
        assertFalse(File(game, "eboot.bin").exists())
        assertTrue(File(game, "sce_sys/param.sfo").readBytes().contentEquals(sfo))
        val mismatch = GameInstallVerifier.verifyTreeForRegistration(game, "CUSA00000") {
            arrayOf(buildMinimalSfo(mapOf("TITLE_ID" to "CUSA00001")), byteArrayOf())
        }
        assertTrue(mismatch is GameInstallVerifier.VerifyResult.Fail)
        assertTrue(GameInstallVerifier.verifyTreeForRegistration(game, "CUSA00000") { null }
            is GameInstallVerifier.VerifyResult.Fail)
    }
}
