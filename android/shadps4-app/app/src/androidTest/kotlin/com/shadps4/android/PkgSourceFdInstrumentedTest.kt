package com.shadps4.android

import android.net.Uri
import android.system.Os
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.data.openIndependentPkgFd
import java.io.File
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class PkgSourceFdInstrumentedTest {
    @Test fun readsAndSeeksAfterProviderProcessExits() {
        val resolver = InstrumentationRegistry.getInstrumentation().targetContext.contentResolver
        val uri = Uri.parse("content://com.shadps4.android.test.pkgfd/source")
        resolver.openIndependentPkgFd(uri).use { descriptor ->
            // Framework releases closed provider references asynchronously (currently 1 s).
            Thread.sleep(2000)
            val pid = resolver.acquireUnstableContentProviderClient(uri)!!.use { client ->
                client.call("exit", null, null)!!.getInt("pid")
            }
            Thread.sleep(2000)
            assertEquals("fixture process must have exited", false, File("/proc/$pid").exists())
            val bytes = ByteArray(32)
            assertEquals(bytes.size, Os.pread(descriptor.fileDescriptor, bytes, 0, bytes.size, 4050))
            assertArrayEquals(ByteArray(32) { ((4050 + it) % 251).toByte() }, bytes)
            assertEquals(bytes.size, Os.pread(descriptor.fileDescriptor, bytes, 0, bytes.size, 0))
            assertArrayEquals(ByteArray(32) { it.toByte() }, bytes)
        }
    }
}
