package com.shadps4.android

/** Internal immersive session Activity; Bluetooth controllers use MainActivity's existing dispatch. */
class OpenXrActivity : MainActivity() {
    override val openXrEnabled = true

    private val xrPermissions = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestMultiplePermissions(),
    ) { results ->
        // Denied or unavailable: invalid gaze uses fixed foveation, and without hands the
        // DualShock 4 stays where it was last seen.
        results.forEach { (permission, granted) ->
            android.util.Log.i("OpenXR", "$permission granted=$granted")
        }
    }

    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        super.onCreate(savedInstanceState)
        if (isFinishing) return
        val device = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} ${android.os.Build.DEVICE}".lowercase()
        val vendor = when {
            "swan" in device || "pico" in device -> "com.picovr.permission"
            "quest" in device || "oculus" in device -> "com.oculus.permission"
            else -> return
        }
        // Ask once each. Users can enable them later through Android's app permission settings.
        val preferences = getSharedPreferences("xr_permissions", MODE_PRIVATE)
        val missing = listOf("$vendor.EYE_TRACKING", "$vendor.HAND_TRACKING").filter { permission ->
            checkSelfPermission(permission) != android.content.pm.PackageManager.PERMISSION_GRANTED &&
                !preferences.getBoolean(permission, false) &&
                runCatching { packageManager.getPermissionInfo(permission, 0) }.isSuccess
        }
        if (missing.isNotEmpty()) {
            preferences.edit().apply { missing.forEach { putBoolean(it, true) } }.apply()
            xrPermissions.launch(missing.toTypedArray())
        }
    }

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        if (com.shadps4.android.runtime.session.NativeFexSession.nativeXrErrorKey(
                event.keyCode, event.action == android.view.KeyEvent.ACTION_DOWN)) return true
        return super.dispatchKeyEvent(event)
    }
}
