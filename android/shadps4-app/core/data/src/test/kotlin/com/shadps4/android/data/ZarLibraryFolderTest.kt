package com.shadps4.android.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ZarLibraryFolderTest {
    @Test
    fun `absolute paths are kept and tidied`() {
        assertEquals("/sdcard/game/ps4/roms", ZarLibraryFolder.normalize("/sdcard/game/ps4/roms"))
        assertEquals("/sdcard/game/ps4/roms", ZarLibraryFolder.normalize("  /sdcard/game/ps4/roms/  "))
        assertEquals("/sdcard/game/ps4/roms", ZarLibraryFolder.normalize("/sdcard//game/./ps4/roms"))
        assertEquals("/sdcard/roms", ZarLibraryFolder.normalize("/sdcard/game/../roms"))
        assertEquals("/sdcard/roms", ZarLibraryFolder.normalize("file:///sdcard/roms"))
        assertEquals("/", ZarLibraryFolder.normalize("/"))
    }

    @Test
    fun `blank relative and escaping paths are rejected`() {
        assertNull(ZarLibraryFolder.normalize(null))
        assertNull(ZarLibraryFolder.normalize("   "))
        assertNull(ZarLibraryFolder.normalize("roms"))
        assertNull(ZarLibraryFolder.normalize("content://com.android.externalstorage.documents/tree/primary%3Aroms"))
        assertNull(ZarLibraryFolder.normalize("/.."))
    }

    @Test
    fun `a storage tree uri becomes the folder it names`() {
        assertEquals(
            "/sdcard/game/ps4/roms",
            ZarLibraryFolder.fromTreeUri(
                "content://com.android.externalstorage.documents/tree/primary%3Agame%2Fps4%2Froms",
                primaryRoot = "/sdcard",
            ),
        )
        assertEquals(
            "/sdcard",
            ZarLibraryFolder.fromTreeUri(
                "content://com.android.externalstorage.documents/tree/primary%3A",
                primaryRoot = "/sdcard",
            ),
        )
        assertEquals(
            "/storage/1A2B-3C4D/roms",
            ZarLibraryFolder.fromTreeUri(
                "content://com.android.externalstorage.documents/tree/1A2B-3C4D%3Aroms",
                primaryRoot = "/sdcard",
            ),
        )
    }

    @Test
    fun `trees that are not device storage are refused`() {
        assertNull(ZarLibraryFolder.fromTreeUri(null))
        assertNull(ZarLibraryFolder.fromTreeUri("content://com.google.android.apps.docs/tree/abc"))
        assertNull(
            ZarLibraryFolder.fromTreeUri(
                "content://com.android.externalstorage.documents/tree/%3Aroms",
                primaryRoot = "/sdcard",
            ),
        )
    }
}
