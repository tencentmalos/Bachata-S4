package com.shadps4.android

import android.app.Instrumentation
import android.content.Intent
import android.view.SurfaceHolder
import android.view.SurfaceView
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.RuntimeSurface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Real SurfaceView producer for app/runtime acceptance; no offscreen substitute. */
class RuntimeTestSurface(private val instrumentation: Instrumentation) : AutoCloseable {
    private val activity = instrumentation.startActivitySync(
        Intent(instrumentation.targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    private lateinit var view: SurfaceView
    val surface get() = view.holder.surface
    init {
        val ready = CountDownLatch(1)
        instrumentation.runOnMainSync {
            view = SurfaceView(activity)
            view.holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceCreated(holder: SurfaceHolder) = Unit
                override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                    if (width > 0 && height > 0) {
                        ManagedSession.attachSurface(RuntimeSurface(holder.surface, width, height))
                        ready.countDown()
                    }
                }
                override fun surfaceDestroyed(holder: SurfaceHolder) {
                    ManagedSession.detachSurface(holder.surface)
                }
            })
            activity.setContentView(view)
        }
        if (!ready.await(10, TimeUnit.SECONDS)) {
            close()
            error("Runtime SurfaceView did not become ready")
        }
    }
    override fun close() {
        instrumentation.runOnMainSync { activity.finish() }
        instrumentation.waitForIdleSync()
    }
}
