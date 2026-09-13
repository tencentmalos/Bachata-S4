package com.shadps4.android.runtime.session

import android.content.Context
import java.io.File
import java.security.MessageDigest

/** The native bionic profile is separate from historical glibc runtime bundles. */
object AndroidTurnip {
    private const val SHA = "fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09"
    private const val LIBRARY = "vulkan.ad07xx.so"

    init { System.loadLibrary("shadps4_fex_session") }

    data class Paths(val hooks: String, val driver: String)

    @Synchronized fun prepare(context: Context): Paths {
        val root = File(context.filesDir, "native-drivers/$SHA").apply { mkdirs() }
        val target = File(root, LIBRARY)
        if (!target.isFile || sha(target) != SHA) {
            // Do not replace a potentially mapped ELF at this immutable identity.
            check(!target.exists()) { "Installed Turnip was modified; restart after removing the corrupt profile" }
            val temporary = File.createTempFile("turnip-", ".tmp", root)
            try {
                context.assets.open("native-turnip/$LIBRARY").use { input ->
                    temporary.outputStream().use { output -> input.copyTo(output) }
                }
                check(sha(temporary) == SHA) { "Bundled bionic Turnip hash mismatch" }
                check(temporary.setReadOnly()) { "Cannot protect Turnip ELF" }
                check(temporary.renameTo(target)) { "Cannot install Turnip ELF" }
            } finally { temporary.delete() }
        }
        return Paths(context.applicationInfo.nativeLibraryDir, root.absolutePath)
    }

    private fun sha(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(65536)
            while (true) {
                val size = input.read(buffer)
                if (size < 0) break
                digest.update(buffer, 0, size)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
    }

    /** Returns verified physical-device identity; failure is a Java exception. */
    external fun nativeLoad(hooks: String, driver: String): String

    /** Runs the production Vulkan Instance/Swapchain constructors on this Surface. */
    external fun nativeInspectSurface(hooks: String, driver: String, surface: android.view.Surface): String
}
