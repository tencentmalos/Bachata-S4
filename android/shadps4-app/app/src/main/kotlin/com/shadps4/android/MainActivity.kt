package com.shadps4.android

import android.annotation.SuppressLint
import android.content.Intent
import android.os.Bundle
import android.os.Build
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableIntStateOf
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
    private var openLastGameRequest by mutableIntStateOf(0)

    private fun consumeLaunchIntent(value: Intent) {
        val requested = value.getBooleanExtra("open_last_game", false) ||
            value.getBooleanExtra("--open_last_game", false)
        value.removeExtra("open_last_game")
        value.removeExtra("--open_last_game")
        if (requested) openLastGameRequest++
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        consumeLaunchIntent(intent)
    }

    override fun dump(prefix: String, fd: java.io.FileDescriptor?, writer: java.io.PrintWriter,
                      args: Array<out String>?) {
        if (args?.singleOrNull() in listOf("--open_last_game", "open_last_game")) {
            runOnUiThread { openLastGameRequest++ }
            writer.println("open_last_game queued; result in OpenLastGame logcat")
            return
        }
        super.dump(prefix, fd, writer, args)
    }

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
        if (savedInstanceState == null) consumeLaunchIntent(intent)
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
                    BachataNavHost(
                        startDestination = initialRouteForSoc(Build.SOC_MODEL, isRuntimeInstalled),
                        openLastGameRequest = openLastGameRequest,
                    )
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
