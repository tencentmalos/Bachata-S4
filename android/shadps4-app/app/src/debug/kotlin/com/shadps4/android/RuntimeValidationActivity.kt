package com.shadps4.android

/** Debug APK's ordinary app-UID Surface host. Avoid launcher settings/permissions
 * recreating the Activity under a native validation session. No native test backend. */
class RuntimeValidationActivity : android.app.Activity() {
    private var systemBars: com.shadps4.android.feature.session.SessionSystemBars? = null

    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        super.onCreate(savedInstanceState)
        systemBars = com.shadps4.android.feature.session.SessionSystemBars(window)
    }

    override fun onDestroy() {
        systemBars?.close()
        systemBars = null
        super.onDestroy()
    }
}
