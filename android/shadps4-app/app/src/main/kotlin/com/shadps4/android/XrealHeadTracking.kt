package com.shadps4.android

import android.app.Activity
import android.util.Log
import com.shadps4.android.runtime.input.NativePad
import spatial.xreal.PoseSample
import spatial.xreal.XrealPoseTracker
import spatial.xreal.XrealSession
import java.io.File
import java.lang.ref.WeakReference

/** Only app/session policy lives here. Foundation owns the isolated SDK worker. */
internal object XrealHeadTracking {
    private var activity = WeakReference<Activity>(null)
    private var permissionRequest: (() -> Unit)? = null
    private var sbsActive = false
    private var tracker: XrealPoseTracker? = null
    private val empty = FloatArray(0)

    fun permissionResult(granted: Boolean) { Log.i("XrealHead", "camera_permission=$granted") }
    fun status(): String = "sbs=$sbsActive foreground=${activity.get() != null} " +
        (tracker?.status() ?: "worker=not_created scans=0 queries=0")

    fun resume(owner: Activity, requestPermission: () -> Unit) {
        activity = WeakReference(owner)
        permissionRequest = requestPermission
        update()
    }

    fun pause(owner: Activity) {
        if (activity.get() !== owner) return
        activity.clear()
        permissionRequest = null
        update()
    }

    fun setSbsActive(active: Boolean) {
        if (sbsActive == active) return
        sbsActive = active
        Log.i("XrealHead", "SBS tracking requested=$active")
        update()
    }

    private fun update() {
        val owner = activity.get().takeIf { sbsActive }
        if (owner != null && tracker == null) {
            tracker = XrealPoseTracker(File(owner.filesDir, "xreal"), XrealSession.TrackingMode.SIX_DOF,
                object : XrealPoseTracker.Listener {
                    override fun sourceActive(active: Boolean) { NativePad.nativeSelectExternalHead(active) }
                    override fun pose(sample: PoseSample, queryStartedNs: Long) {
                        val valid = sample.onePro && sample.poseConfirmed
                        NativePad.nativeUpdateExternalHead(if (valid) sample.rightHandedPose() else empty,
                            valid, valid && sample.trackingType == 0, queryStartedNs)
                    }
                })
        }
        tracker?.setActive(owner, permissionRequest?.let { callback -> Runnable { callback() } })
    }
}
