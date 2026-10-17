package com.shadps4.android

import android.app.Application
import dagger.hilt.android.HiltAndroidApp
import java.io.File

@HiltAndroidApp
class App : Application() {
    override fun onCreate() {
        super.onCreate()
        // A process restart closes every source FD. Remove only abandoned alias
        // directories before any Activity or Service can create a new lease.
        runCatching { File(filesDir, "rom-leases").deleteRecursively() }
            .onFailure { android.util.Log.w("ManagedRom", "Could not clean abandoned leases", it) }
    }
}
