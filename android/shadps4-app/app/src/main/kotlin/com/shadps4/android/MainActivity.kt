package com.shadps4.android

import android.annotation.SuppressLint
import android.os.Bundle
import android.os.Build
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import com.shadps4.android.designsystem.theme.AppTheme
import dagger.hilt.android.AndroidEntryPoint
import androidx.lifecycle.lifecycleScope
import android.view.KeyEvent
import android.view.MotionEvent
import com.shadps4.android.data.LegacyRuntimeSettingsMigration
import com.shadps4.android.data.UiOrientationPreference
import com.shadps4.android.runtime.input.GamepadInputManager
import androidx.activity.enableEdgeToEdge
import javax.inject.Inject
import kotlinx.coroutines.launch

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    @Inject lateinit var legacyRuntimeSettingsMigration: LegacyRuntimeSettingsMigration

    @SuppressLint("RestrictedApi")
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        com.shadps4.android.runtime.input.NativePadBridge.setFocused(hasFocus)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean =
        GamepadInputManager.dispatchKeyEvent(event) || super.dispatchKeyEvent(event)

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean =
        GamepadInputManager.dispatchGenericMotionEvent(event) || super.dispatchGenericMotionEvent(event)

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        val uiOrientation = UiOrientationPreference.read(this)
        requestedOrientation = UiOrientationPreference.toActivityOrientation(uiOrientation)
        lifecycleScope.launch { legacyRuntimeSettingsMigration.migrate() }
        // The FEX CPU backend (libshadps4_fex_session.so) is compiled into the APK, so unlike the
        // reference (which downloaded/extracted a glibc runtime into filesDir/runtime/box64-*),
        // there is no external runtime to install — the backend is always present.
        val isRuntimeInstalled = true
        setContent {
            AppTheme {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    BachataNavHost(startDestination = initialRouteForSoc(Build.SOC_MODEL, isRuntimeInstalled))
                }
            }
        }
    }
}

internal fun initialRouteForSoc(soc: String, isRuntimeInstalled: Boolean): String =
    if (isRuntimeInstalled) {
        BachataRoutes.Library
    } else {
        BachataRoutes.Setup
    }
