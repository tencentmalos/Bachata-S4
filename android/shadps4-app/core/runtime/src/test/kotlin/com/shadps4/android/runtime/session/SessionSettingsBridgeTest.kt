package com.shadps4.android.runtime.session

import org.junit.Assert.*
import org.junit.Test

class SessionSettingsBridgeTest {
    @Test fun passesOnlyCatalogChoicesToTheInstalledListener() {
        val seen = mutableListOf<Pair<String, String>>()
        val listener = SessionSettingsBridge.Listener { spec, value -> seen += spec.id to value }
        SessionSettingsBridge.install(listener)
        try {
            SessionSettingsBridge.onNativeSettingChanged("gpu.frame_limit", "30")
            SessionSettingsBridge.onNativeSettingChanged("gpu.screen_upscaler", "sgsr1")
            SessionSettingsBridge.onNativeSettingChanged("vulkan.pipeline_compile_mode", "sync")
            SessionSettingsBridge.onNativeSettingChanged("gpu.frame_limit", "33") // not a choice
            SessionSettingsBridge.onNativeSettingChanged("gpu.not_a_setting", "1") // unknown key
        } finally {
            SessionSettingsBridge.uninstall(listener)
        }
        assertEquals(
            listOf("gpu.frame_limit" to "30", "gpu.screen_upscaler" to "sgsr1",
                "vulkan.pipeline_compile_mode" to "sync"),
            seen,
        )
        SessionSettingsBridge.onNativeSettingChanged("gpu.frame_limit", "20") // no listener
        assertEquals(3, seen.size)
    }

    @Test fun uninstallKeepsALaterSessionsListener() {
        var later = 0
        val first = SessionSettingsBridge.Listener { _, _ -> }
        val second = SessionSettingsBridge.Listener { _, _ -> later++ }
        SessionSettingsBridge.install(first)
        SessionSettingsBridge.install(second)
        SessionSettingsBridge.uninstall(first)
        SessionSettingsBridge.onNativeSettingChanged("gpu.frame_limit", "40")
        SessionSettingsBridge.uninstall(second)
        assertEquals(1, later)
    }
}
