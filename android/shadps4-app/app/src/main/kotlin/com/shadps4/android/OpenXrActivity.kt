package com.shadps4.android

/** Internal immersive session Activity; Bluetooth controllers use MainActivity's existing dispatch. */
class OpenXrActivity : MainActivity() {
    override val openXrEnabled = true

    private val gazePermission = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestPermission(),
    ) { granted -> android.util.Log.i("OpenXR", "Eye tracking permission granted=$granted; invalid gaze uses fixed foveation") }

    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        super.onCreate(savedInstanceState)
        if (isFinishing) return
        val device = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} ${android.os.Build.DEVICE}".lowercase()
        val permission = when {
            "swan" in device || "pico" in device -> "com.picovr.permission.EYE_TRACKING"
            "quest" in device || "oculus" in device -> "com.oculus.permission.EYE_TRACKING"
            else -> return
        }
        // Ask once. A denied or unavailable sensor always falls back to fixed foveation;
        // users can enable it later through Android's app permission settings.
        val preferences = getSharedPreferences("xr_permissions", MODE_PRIVATE)
        if (checkSelfPermission(permission) != android.content.pm.PackageManager.PERMISSION_GRANTED &&
            !preferences.getBoolean(permission, false)) {
            val declared = runCatching { packageManager.getPermissionInfo(permission, 0) }.isSuccess
            if (declared) {
                preferences.edit().putBoolean(permission, true).apply()
                gazePermission.launch(permission)
            }
        }
    }

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        if (com.shadps4.android.runtime.session.NativeFexSession.nativeXrErrorKey(
                event.keyCode, event.action == android.view.KeyEvent.ACTION_DOWN)) return true
        return super.dispatchKeyEvent(event)
    }
}
