package com.shadps4.android.runtime.pkg

import androidx.test.ext.junit.runners.AndroidJUnit4
import java.io.File
import java.io.RandomAccessFile
import kotlin.test.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * On-device extraction of a real PS4 .pkg using the production native extractor
 * (libbachata_pkg + the PkgRsa JVM callback). Produces the decrypted eboot.bin +
 * sce_sys/param.sfo used by the HN2 guest-execution bring-up.
 *
 * Input PKG must be staged at /data/local/tmp/tmnt_base.pkg (pushed via adb).
 * Output goes to /data/local/tmp/tmnt_extract so the harness/JNI can read it.
 *
 * This is a developer/CI extraction harness, not an app-facing flow (the app imports
 * via SAF). It reuses the exact production extractor so the produced tree matches
 * what a real import yields.
 */
@RunWith(AndroidJUnit4::class)
class PkgExtractInstrumentedTest {

    private val pkgPath = "/data/local/tmp/tmnt_base.pkg"
    private val outPath = "/data/local/tmp/tmnt_extract2"

    @Test
    fun extractTmntBase() {
        val pkg = File(pkgPath)
        org.junit.Assume.assumeTrue("PKG not staged at $pkgPath; skipping", pkg.isFile)

        File(outPath).deleteRecursively()
        File(outPath).mkdirs()

        RandomAccessFile(pkg, "r").use { raf ->
            // Reflect the fd out of the FileDescriptor (no public int accessor).
            val fdField = raf.fd.javaClass.getDeclaredField("descriptor").apply { isAccessible = true }
            val fd = fdField.getInt(raf.fd)

            val probe = PkgExtractor.nativeProbe(fd)
            println("PKG probe: contentId=${probe.contentId} status=${probe.status} pfs=${probe.pfsImageSize} msg=${probe.message}")
            assertTrue(probe.status == PkgStatus.OK || probe.status == PkgStatus.NEED_PASSCODE,
                "probe status=${probe.status} msg=${probe.message}")

            // Reset to start for the extract read.
            raf.seek(0)
            val result = PkgExtractor.nativeExtract(fd, outPath, null) { done, total, file ->
                if (file.endsWith("eboot.bin")) println("extracting eboot: $done/$total")
            }
            println("PKG extract: status=${result.status} contentId=${result.contentId} msg=${result.message}")
            assertTrue(result.status == PkgStatus.OK, "extract status=${result.status} msg=${result.message}")
        }

        val eboot = File(outPath, "eboot.bin")
        val sfo = File(outPath, "sce_sys/param.sfo")
        println("eboot exists=${eboot.isFile} size=${eboot.length()}; param.sfo exists=${sfo.isFile} size=${sfo.length()}")
        assertTrue(eboot.isFile && eboot.length() > 0, "eboot.bin missing/empty")
        assertTrue(sfo.isFile && sfo.length() > 0, "param.sfo missing/empty")

        // Make the extracted tree world-readable so the adb-shell HN2 harness (a
        // different UID) can open eboot.bin. The device is SELinux-permissive here.
        fun chmodRec(f: File) {
            f.setReadable(true, false)
            f.setExecutable(true, false) // dir traversal
            if (f.isDirectory) f.listFiles()?.forEach { chmodRec(it) }
        }
        chmodRec(File(outPath))
        println("staged extract tree world-readable at $outPath")
    }
}
