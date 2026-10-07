package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class XrDs4PoseTest {
    private fun profile(value: String) = RuntimeProfile(values = mapOf(XrDs4Pose.ID to JsonPrimitive(value)))

    @Test fun defaultOffAndGameOverride() {
        assertEquals(XrDs4Pose.OFF, XrDs4Pose.resolve(RuntimeProfile(), RuntimeProfile()))
        assertEquals(XrDs4Pose.RIGHT, XrDs4Pose.resolve(profile("right"), RuntimeProfile()))
        assertEquals(XrDs4Pose.BOTH, XrDs4Pose.resolve(profile("right"), profile("both")))
        assertEquals(XrDs4Pose.OFF, XrDs4Pose.resolve(profile("both"), profile("off")))
        assertEquals(XrDs4Pose.HANDS, XrDs4Pose.resolve(RuntimeProfile(), profile("hands")))
        val specs = RuntimeSettingCatalog.loadAndroidSettings()
        val spec = specs.single { it.id == XrDs4Pose.ID }
        assertEquals(listOf("off", "right", "both", "hands"), spec.choices)
        assertTrue(spec.restartRequired)
        assertEquals(SettingScope.GLOBAL_AND_GAME, spec.scope)
        val imported = ShadPs4JsonCodec.applyRawJson(RuntimeProfile(),
            """{"Input":{"xr_ds4_pose":"both"}}""", specs)
        assertEquals(XrDs4Pose.BOTH, XrDs4Pose.resolve(RuntimeProfile(), imported))
    }

    @Test fun invalidValueIsRejected() {
        assertThrows(IllegalArgumentException::class.java) {
            XrDs4Pose.resolve(profile("left"), RuntimeProfile())
        }
    }
}
