package com.shadps4.android

import android.app.Instrumentation
import android.content.Intent
import android.view.SurfaceHolder
import android.view.SurfaceView
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.RuntimeSurface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Real SurfaceView producer for app/runtime acceptance; no offscreen substitute. */
class RuntimeTestSurface(private val instrumentation: Instrumentation) : AutoCloseable {
    private val activity = instrumentation.startActivitySync(
        Intent(instrumentation.targetContext, RuntimeValidationActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    private lateinit var view: SurfaceView
    private val destroyed = CountDownLatch(1)
    private val closed = AtomicBoolean()
    val surface get() = view.holder.surface
    init {
        val ready = CountDownLatch(1)
        instrumentation.runOnMainSync {
            view = SurfaceView(activity).apply { keepScreenOn = true }
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
                    destroyed.countDown()
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
        if (!closed.compareAndSet(false, true)) return
        instrumentation.runOnMainSync { activity.finish() }
        check(destroyed.await(5, TimeUnit.SECONDS)) { "Surface destruction did not complete" }
        // SurfaceView calls surfaceDestroyed before releasing its Java Surface.
        // The callback latch alone does not establish completion of that release.
        val deadline = android.os.SystemClock.uptimeMillis() + 5000
        while (surface.isValid && android.os.SystemClock.uptimeMillis() < deadline)
            android.os.SystemClock.sleep(5)
        check(!surface.isValid) { "Surface release did not complete" }
    }
}
