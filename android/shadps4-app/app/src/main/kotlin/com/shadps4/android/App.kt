package com.shadps4.android

import android.app.Activity
import android.app.Application
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.PauseReason
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
        registerActivityLifecycleCallbacks(BackgroundPause())
    }
}

/**
 * Holds the running game while no activity of the app is visible, so the guest stops using the
 * CPU, GPU and audio device in the background. The 2D session screen turns the hold into its
 * menu when the app returns (the player resumes from there); the headset view resumes at once.
 * Main thread only.
 */
private class BackgroundPause : Application.ActivityLifecycleCallbacks {
    private val handler = Handler(Looper.getMainLooper())
    private var started = 0

    // A rotation or a hand-off between the 2D and the headset activity stops one activity
    // around the start of the next: only a stretch without any started activity counts.
    private val enterBackground = Runnable {
        if (started == 0) ManagedSession.requestPause(PauseReason.Background)
    }

    // Nothing visible took the hold over (no session screen on top): run again.
    private val releaseUnclaimed = Runnable {
        if (started > 0) ManagedSession.releasePause(PauseReason.Background)
    }

    override fun onActivityStarted(activity: Activity) {
        handler.removeCallbacks(enterBackground)
        if (++started != 1 || PauseReason.Background !in ManagedSession.pauseReasons.value) return
        if (activity is OpenXrActivity) {
            ManagedSession.releasePause(PauseReason.Background)
        } else {
            handler.removeCallbacks(releaseUnclaimed)
            handler.postDelayed(releaseUnclaimed, UNCLAIMED_RELEASE_MS)
        }
    }

    override fun onActivityStopped(activity: Activity) {
        if (started > 0 && --started == 0) {
            handler.removeCallbacks(releaseUnclaimed)
            handler.postDelayed(enterBackground, BACKGROUND_DELAY_MS)
        }
    }

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = Unit
    override fun onActivityResumed(activity: Activity) = Unit
    override fun onActivityPaused(activity: Activity) = Unit
    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit
    override fun onActivityDestroyed(activity: Activity) = Unit

    private companion object {
        const val BACKGROUND_DELAY_MS = 500L
        const val UNCLAIMED_RELEASE_MS = 3000L
    }
}
