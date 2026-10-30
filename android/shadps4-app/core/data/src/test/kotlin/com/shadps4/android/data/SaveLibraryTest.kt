package com.shadps4.android.data

import java.io.File
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SaveLibraryTest {
    @get:Rule val temp = TemporaryFolder()
    private fun payload(user: String = "1000", title: String = "CUSA01740"): File =
        File(temp.root, "home/$user/savedata/$title/slot1").apply {
            mkdirs(); resolve("data.bin").writeBytes(byteArrayOf(1, 2, 3))
            resolve("sce_sys").mkdirs()
            resolve("sce_sys/param.sfo").writeBytes(buildMinimalSfo(mapOf("TITLE" to "游戏", "SUBTITLE" to "第一关")))
        }
    private fun library() = SaveLibrary(File(temp.root, "home"), File(temp.root, "notes"))

    @Test fun chineseNotesPersistWithoutChangingPayloadAndAreIsolatedByIdentity() {
        val dir = payload()
        payload("1001")
        payload(title = "CUSA00002")
        val before = dir.walkTopDown().filter { it.isFile }.associate { it.path to (it.readBytes().toList() to it.lastModified()) }
        val lib = library()
        val save = lib.list("CUSA01740").first { it.userId == "1000" }
        assertEquals("第一关", save.subtitle)
        assertEquals(before.values.sumOf { it.first.size }.toLong(), save.bytes)
        lib.writeNote(save, "战神：到达新区域\n下次继续 👋")
        val reread = library().list("CUSA01740")
        assertEquals("战神：到达新区域\n下次继续 👋", reread.first { it.userId == "1000" }.note)
        assertEquals("", reread.first { it.userId == "1001" }.note)
        assertEquals("", lib.list("CUSA00002").single().note)
        assertEquals(before, dir.walkTopDown().filter { it.isFile }.associate { it.path to (it.readBytes().toList() to it.lastModified()) })
        lib.writeNote(save, "")
        assertEquals("", library().list("CUSA01740").first { it.userId == "1000" }.note)
        assertTrue(File(temp.root, "notes").listFiles()!!.isEmpty())
    }

    @Test fun linksAndRemovedOrForgedEntriesCannotRedirectAccess() {
        val dir = payload()
        val outside = temp.newFolder("outside").apply { resolve("secret").writeText("private") }
        Files.createSymbolicLink(dir.resolve("linked").toPath(), outside.toPath())
        val lib = library()
        val save = lib.list("CUSA01740").single()
        assertFalse(save.files.any { it.path.contains("secret") })
        assertTrue(save.warnings.isNotEmpty())
        assertThrows(IllegalArgumentException::class.java) { lib.list("../outside") }
        assertThrows(IllegalArgumentException::class.java) { lib.writeNote(save.copy(directory = "../outside"), "bad") }
        Files.delete(dir.resolve("linked").toPath())
        dir.deleteRecursively()
        assertThrows(IllegalArgumentException::class.java) { lib.writeNote(save, "gone") }
        assertEquals("private", outside.resolve("secret").readText())
    }

    @Test fun brokenMetadataDoesNotHideSaveAndOversizedNotePreservesPrevious() {
        payload().resolve("sce_sys/param.sfo").writeText("invalid")
        val lib = library()
        val save = lib.list("CUSA01740").single()
        assertNull(save.title)
        lib.writeNote(save, "原备注")
        assertThrows(IllegalArgumentException::class.java) { lib.writeNote(save, "x".repeat(4097)) }
        assertEquals("原备注", library().list("CUSA01740").single().note)
    }

    @Test fun sharedSaveTitleIsReadFromGameSfo() {
        val sfo = ParamSfoReader.parse(buildMinimalSfo(mapOf("TITLE_ID" to "CUSA00001",
            "INSTALL_DIR_SAVEDATA" to "CUSA00002")))
        assertEquals("CUSA00001", sfo.titleId)
        assertEquals("CUSA00002", sfo.saveTitleId)
    }
}
