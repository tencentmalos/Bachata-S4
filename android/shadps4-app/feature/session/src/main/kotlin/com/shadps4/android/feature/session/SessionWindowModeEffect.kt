package com.shadps4.android.feature.session

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import com.shadps4.android.data.UiOrientationPreference

@Composable
fun SessionWindowModeEffect() {
    val context = LocalContext.current
    val view = LocalView.current
    DisposableEffect(context, view) {
        val activity = context.findActivity()
        val systemBars = activity?.window?.let { SessionSystemBars(it) }
        activity?.requestedOrientation = SessionWindowMode.ImmersiveLandscape.orientation
        onDispose {
            val restored = UiOrientationPreference.read(context)
            activity?.requestedOrientation = UiOrientationPreference.toActivityOrientation(restored)
            systemBars?.close()
        }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.takeUnless { it === this }?.findActivity()
    else -> null
}
