package com.shadps4.android

import android.content.ContextWrapper
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.session.FirmwareModules
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class FirmwarePreservationInstrumentedTest {
    @Test fun omittedBundlePreservesInstalledModulesAndOwnership() {
        // The test APK has an empty index. Use its assets with an isolated filesDir.
        val testContext = InstrumentationRegistry.getInstrumentation().context
        val cache = InstrumentationRegistry.getInstrumentation().targetContext.cacheDir
        val root = Files.createTempDirectory(cache.toPath(), "firmware-test").toFile()
        val context = object : ContextWrapper(testContext) {
            override fun getFilesDir() = root
        }
        try {
            val directory = File(root, "host/sys_modules").apply { mkdirs() }
            val bundled = File(directory, "libSceNgs2.sprx").apply { writeBytes(byteArrayOf(1, 2, 3)) }
            val user = File(directory, "user.sprx").apply { writeBytes(byteArrayOf(4, 5, 6)) }
            val hash = MessageDigest.getInstance("SHA-256").digest(bundled.readBytes())
                .joinToString("") { "%02x".format(it.toInt() and 255) }
            val record = File(directory, ".bundled-firmware.json")
                .apply { writeText("{\"libSceNgs2.sprx\":\"$hash\"}\n") }
            val before = directory.listFiles()!!.associate { it.name to it.readBytes() }
            val result = FirmwareModules.prepare(context)
            assertEquals(0, result.removed)
            assertEquals(before.keys, directory.listFiles()!!.map { it.name }.toSet())
            for (file in listOf(bundled, user, record)) assertArrayEquals(before[file.name], file.readBytes())
        } finally {
            root.deleteRecursively()
        }
    }
}
