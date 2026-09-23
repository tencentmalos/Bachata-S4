package com.shadps4.android.data

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ExternalArchiveLibraryTest {
    @get:Rule
    val temp = TemporaryFolder()

    private fun metadata(titleId: String): Array<ByteArray> = arrayOf(
        buildMinimalSfo(mapOf("TITLE" to "Game $titleId", "TITLE_ID" to titleId)),
        byteArrayOf(1, 2, 3),
    )

    private fun archive(folder: File, name: String, bytes: Int = 64): File =
        File(folder, name).apply { writeBytes(ByteArray(bytes) { it.toByte() }) }

    @Test
    fun `overlay archives are not registered on their own`() {
        val folder = temp.newFolder("roms")
        archive(folder, "CUSA03023.zar")
        archive(folder, "CUSA03023-UPD.zar")
        archive(folder, "CUSA03023-DLC.zar")
        archive(folder, "CUSA12878-UPDATE.zar")
        archive(folder, "CUSA12878-patch.zar")
        archive(folder, "CUSA12878-mods.zar")
        archive(folder, "CUSA12878.zar")
        archive(folder, "notes.txt")

        val bases = ExternalArchiveLibrary.baseArchives(folder).map { it.name }
        assertEquals(listOf("CUSA03023.zar", "CUSA12878.zar"), bases)
    }

    @Test
    fun `sync links a base archive and keeps the game data outside app storage`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        val base = archive(folder, "CUSA03023.zar", bytes = 4096)

        val result = ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })

        assertTrue(result.failures.toString(), result.failures.isEmpty())
        assertEquals(listOf("CUSA03023"), result.linked.map { it.id })
        val dir = File(filesDir, "games/CUSA03023")
        assertTrue(File(dir, "sce_sys/param.sfo").isFile)
        assertEquals(base.absolutePath, ArchiveLinkIo.read(dir)?.path)
        // Only metadata is cached; the archive itself is never copied.
        assertFalse(File(dir, "CUSA03023.zar").exists())
        assertTrue(dir.walkTopDown().filter { it.isFile }.sumOf { it.length() } < base.length())
        assertEquals(base.absolutePath, GameInstallVerifier.executableFile(dir).absolutePath)
        assertTrue(GameInstallVerifier.canLaunch(filesDir, "games/CUSA03023"))
        assertEquals(ExternalArchiveLibrary.MODE, InstallManifestIo.read(dir)?.mode)
    }

    @Test
    fun `second sync reuses the link without inspecting the archive again`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        archive(folder, "CUSA03023.zar")
        ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })

        var inspected = 0
        val again = ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = {
            inspected++
            metadata("CUSA03023")
        })

        assertEquals(0, inspected)
        assertEquals(listOf("CUSA03023"), again.linked.map { it.id })
    }

    @Test
    fun `a replaced archive is inspected again and relinked`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        val base = archive(folder, "CUSA03023.zar")
        ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })

        base.writeBytes(ByteArray(8192) { 7 })
        base.setLastModified(base.lastModified() + 5_000L)
        var inspected = 0
        ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = {
            inspected++
            metadata("CUSA03023")
        })

        assertEquals(1, inspected)
        val link = ArchiveLinkIo.read(File(filesDir, "games/CUSA03023"))
        assertEquals(base.length(), link?.bytes)
    }

    @Test
    fun `clearing the folder removes links but keeps installed games`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        archive(folder, "CUSA03023.zar")
        ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })
        val installed = File(filesDir, "games/CUSA50828").apply { mkdirs() }
        File(installed, "eboot.bin").writeBytes(ByteArray(16))

        val result = ExternalArchiveLibrary.sync(filesDir, null, inspectArchive = { metadata("CUSA03023") })

        assertEquals(listOf("CUSA03023"), result.removed)
        assertFalse(File(filesDir, "games/CUSA03023").exists())
        assertTrue(File(installed, "eboot.bin").isFile)
    }

    @Test
    fun `a deleted archive drops its link`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        val base = archive(folder, "CUSA03023.zar")
        ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })
        assertTrue(base.delete())

        val result = ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })

        assertEquals(listOf("CUSA03023"), result.removed)
        assertFalse(GameInstallVerifier.canLaunch(filesDir, "games/CUSA03023"))
    }

    @Test
    fun `an installed copy is never replaced by a link`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        archive(folder, "CUSA03023.zar")
        val installed = File(filesDir, "games/CUSA03023").apply { mkdirs() }
        File(installed, "eboot.bin").writeBytes(ByteArray(16))

        val result = ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })

        assertTrue(result.linked.isEmpty())
        assertEquals(1, result.failures.size)
        assertNull(ArchiveLinkIo.read(installed))
        assertTrue(File(installed, "eboot.bin").isFile)
    }

    @Test
    fun `an archive whose title id does not parse is reported and leaves nothing behind`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        archive(folder, "broken.zar")

        val result = ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { null })

        assertTrue(result.linked.isEmpty())
        assertEquals(setOf("broken.zar"), result.failures.keys)
        assertTrue(File(filesDir, "games").listFiles().orEmpty().isEmpty())
    }

    @Test
    fun `the link directory is named by the archive title id, not the file name`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        archive(folder, "bloodborne.zar")

        ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })

        assertTrue(File(filesDir, "games/CUSA03023").isDirectory)
        assertFalse(File(filesDir, "games/bloodborne").exists())
    }

    @Test
    fun `a link that points outside the configured folder is dropped`() {
        val filesDir = temp.newFolder("files")
        val folder = temp.newFolder("roms")
        val other = temp.newFolder("elsewhere")
        val base = archive(other, "CUSA03023.zar")
        ExternalArchiveLibrary.sync(filesDir, other, inspectArchive = { metadata("CUSA03023") })
        assertTrue(base.isFile)

        val result = ExternalArchiveLibrary.sync(filesDir, folder, inspectArchive = { metadata("CUSA03023") })

        assertEquals(listOf("CUSA03023"), result.removed)
        assertTrue(base.isFile)
    }
}
