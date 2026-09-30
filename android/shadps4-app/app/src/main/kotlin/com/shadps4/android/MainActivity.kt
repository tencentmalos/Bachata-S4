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
import androidx.compose.runtime.mutableStateOf
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
open class MainActivity : ComponentActivity() {
    internal open val openXrEnabled = false
    @Inject lateinit var legacyRuntimeSettingsMigration: LegacyRuntimeSettingsMigration
    private var requestedGameId by mutableStateOf<String?>(null)
    private var openLastGameRequest by mutableIntStateOf(0)

    private fun consumeLaunchIntent(value: Intent) {
        val requested = value.getBooleanExtra("open_last_game", false) ||
            value.getBooleanExtra("--open_last_game", false)
        value.removeExtra("open_last_game")
        value.removeExtra("--open_last_game")
        val direct = value.getStringExtra(DirectGameLaunchRequest.EXTRA_GAME_ID)
        value.removeExtra(DirectGameLaunchRequest.EXTRA_GAME_ID)
        if (direct != null) requestedGameId = direct
        else if (requested) openLastGameRequest++
    }

    override fun onResume() {
        super.onResume()
        if (openXrEnabled) com.shadps4.android.runtime.input.NativePad.nativeOpenXrForeground(this, true)
    }

    override fun onPause() {
        if (openXrEnabled) com.shadps4.android.runtime.input.NativePad.nativeOpenXrForeground(this, false)
        super.onPause()
    }

    override fun onDestroy() {
        com.shadps4.android.runtime.input.NativePad.nativeReleaseOpenXr(this)
        super.onDestroy()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString("pending_game", requestedGameId)
        super.onSaveInstanceState(outState)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        consumeLaunchIntent(intent)
    }

    override fun dump(prefix: String, fd: java.io.FileDescriptor?, writer: java.io.PrintWriter,
                      args: Array<out String>?) {
        // The same Foundation registry is available from Library, before a
        // guest session/service exists. No game launch is needed to export an
        // installed title that currently fails during Prepare.
        if (BuildConfig.DEBUG && args?.firstOrNull() == "debugbus") {
            try {
                val ready = com.shadps4.android.runtime.input.NativePad.nativeInitializeHost(
                    java.io.File(filesDir, "host").absolutePath, applicationContext)
                if (!ready) {
                    writer.println("debug-command-error: host paths unavailable")
                    return
                }
                writer.println(com.shadps4.android.runtime.session.NativeFexSession.nativeDebugCommand(
                    args.drop(1).joinToString(" ")))
            } catch (t: Throwable) {
                writer.println("debug-command-error: ${t.message}")
            }
            return
        }
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
        val nativePad = com.shadps4.android.runtime.input.NativePad
        check(nativePad.nativeInitializeHost(java.io.File(filesDir, "host").absolutePath, applicationContext))
        // A launcher tap must bring the running mode forward, never reconfigure its Vulkan device.
        if (com.shadps4.android.runtime.session.NativeFexSession.nativeCurrentGeneration() != 0L &&
            nativePad.nativeIsOpenXrConfigured() != openXrEnabled) {
            startActivity(Intent(this, if (openXrEnabled) MainActivity::class.java else OpenXrActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
            finish()
            return
        }
        check(nativePad.nativeConfigureOpenXr(this, openXrEnabled))
        requestedGameId = savedInstanceState?.getString("pending_game")
        if (savedInstanceState == null) consumeLaunchIntent(intent)
        val uiOrientation = UiOrientationPreference.read(this)
        // A recreated session already owns its landscape request. Reapplying the
        // library preference here can bounce portrait/landscape indefinitely.
        if (savedInstanceState == null && !openXrEnabled) {
            requestedOrientation = UiOrientationPreference.toActivityOrientation(uiOrientation)
        }
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
                        requestedGameId = requestedGameId,
                        onGameRequestConsumed = { requestedGameId = null },
                        openXrEnabled = openXrEnabled,
                        exitXrToLibrary = {
                            startActivity(Intent(this, MainActivity::class.java))
                            finish()
                        },
                        switchDisplayActivity = { gameId, xr ->
                            startActivity(Intent(this, if (xr) OpenXrActivity::class.java else MainActivity::class.java)
                                .putExtra(DirectGameLaunchRequest.EXTRA_GAME_ID, gameId))
                            finish()
                        },
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
