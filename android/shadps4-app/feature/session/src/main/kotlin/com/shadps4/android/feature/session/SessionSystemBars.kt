package com.shadps4.android.feature.session

import android.view.ViewTreeObserver
import android.view.Window
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/** One visible session owns immersive mode, including focus/Insets control recovery.
 * Both the production Compose screen and the debug Surface host use this policy.
 * The app window is edge-to-edge; transient system bars overlay it without resizing
 * the native Surface. A swipe can still reveal Android's navigation controls.
 */
class SessionSystemBars(window: Window) : AutoCloseable {
    private val decor = window.decorView
    private val controller = WindowCompat.getInsetsController(window, decor)
    private val originalBehavior = controller.systemBarsBehavior
    private val bars = WindowInsetsCompat.Type.systemBars()
    private val initiallyVisible = ViewCompat.getRootWindowInsets(decor)?.let { insets ->
        listOf(WindowInsetsCompat.Type.statusBars(), WindowInsetsCompat.Type.navigationBars(),
            WindowInsetsCompat.Type.captionBar()).fold(0) { mask, type ->
            if (insets.isVisible(type)) mask or type else mask
        }
    } ?: bars
    private var closed = false
    private val focusListener = ViewTreeObserver.OnWindowFocusChangeListener { focused ->
        if (focused) hide()
    }
    private val controlListener = WindowInsetsControllerCompat.OnControllableInsetsChangedListener { _, types ->
        if (types and bars != 0) hide()
    }

    init {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        decor.viewTreeObserver.addOnWindowFocusChangeListener(focusListener)
        controller.addOnControllableInsetsChangedListener(controlListener)
        hide()
    }

    private fun hide() {
        if (closed) return
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller.hide(bars)
    }

    override fun close() {
        if (closed) return
        closed = true
        decor.viewTreeObserver.removeOnWindowFocusChangeListener(focusListener)
        controller.removeOnControllableInsetsChangedListener(controlListener)
        controller.systemBarsBehavior = originalBehavior
        controller.hide(bars and initiallyVisible.inv())
        controller.show(initiallyVisible)
    }
}
