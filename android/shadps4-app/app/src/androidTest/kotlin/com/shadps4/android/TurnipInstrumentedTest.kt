package com.shadps4.android

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.session.AndroidTurnip
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import android.content.Intent
import android.view.SurfaceHolder
import android.view.SurfaceView
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import com.shadps4.android.runtime.input.NativePad
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class TurnipInstrumentedTest {
    @Test fun productionSwapchainSurvivesThreeSurfaceGenerations() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").absolutePath))
        val paths = AndroidTurnip.prepare(context)
        val identity = NativeFexSession.nativeIdentity()
        repeat(3) { round ->
            val activity = instrumentation.startActivitySync(Intent(context, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            val ready = CountDownLatch(1)
            lateinit var view: SurfaceView
            try {
                instrumentation.runOnMainSync {
                    view = SurfaceView(activity)
                    view.holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) = Unit
                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                            if (width > 0 && height > 0) ready.countDown()
                        }
                        override fun surfaceDestroyed(holder: SurfaceHolder) = Unit
                    })
                    activity.setContentView(view)
                }
                assertTrue("Surface ready", ready.await(10, TimeUnit.SECONDS))
                val report = AndroidTurnip.nativeInspectSurface(paths.hooks, paths.driver, view.holder.surface)
                assertTrue(report, report.contains("images=") && report.contains("shaderInt64=1"))
                assertEquals(identity, NativeFexSession.nativeIdentity())
                android.util.Log.i("TurnipAcceptance", "WSI round=${round + 1} $report")
            } finally {
                instrumentation.runOnMainSync { activity.finish() }
                instrumentation.waitForIdleSync()
            }
        }
    }

    @Test fun pinnedBionicDriverLoadsInOrdinaryAppWithoutSystemFallback() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val paths = AndroidTurnip.prepare(context)
        for (hook in listOf("hook_impl", "main_hook", "file_redirect_hook", "gsl_alloc_hook")) {
            assertTrue("packaged $hook", File(paths.hooks, "lib$hook.so").isFile)
        }
        try {
            AndroidTurnip.nativeLoad(paths.hooks, File(context.filesDir, "missing-turnip").path)
            fail("Missing custom driver must not load the system driver")
        } catch (expected: IllegalStateException) {
            assertTrue(expected.message.orEmpty(), expected.message.orEmpty().contains("directory"))
        }
        val identity = NativeFexSession.nativeIdentity()
        val driver = AndroidTurnip.nativeLoad(paths.hooks, paths.driver)
        assertTrue(driver, driver.contains("Turnip"))
        assertTrue(driver, driver.contains("shaderInt64=1"))
        repeat(3) {
            assertEquals(driver, AndroidTurnip.nativeLoad(paths.hooks, paths.driver))
            assertEquals(identity, NativeFexSession.nativeIdentity())
        }
        android.util.Log.i("TurnipAcceptance", "$identity $driver")
    }
}
