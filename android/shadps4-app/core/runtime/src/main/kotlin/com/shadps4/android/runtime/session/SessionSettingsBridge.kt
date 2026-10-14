package com.shadps4.android.runtime.session

import com.shadps4.android.runtime.settings.RuntimeSettingCatalog
import com.shadps4.android.runtime.settings.RuntimeSettingSpec
import com.shadps4.android.runtime.settings.SettingKind

/**
 * Settings the in-game Controls panel (native ImGui) changes while a game runs. Native code calls
 * [onNativeSettingChanged] on its presenting thread; the session service installs a [Listener]
 * that saves the change in the running game's profile. Only ENUM and BOOLEAN catalog settings
 * with a value from their choices are passed on.
 */
object SessionSettingsBridge {
    fun interface Listener {
        fun onSettingChanged(spec: RuntimeSettingSpec, value: String)
    }

    @Volatile private var listener: Listener? = null
    private val catalog: Map<String, RuntimeSettingSpec> by lazy {
        RuntimeSettingCatalog.loadAndroidSettings().associateBy { it.id }
    }

    fun install(listener: Listener) {
        this.listener = listener
    }

    /** Clears [listener] unless a later session has installed its own. */
    @Synchronized
    fun uninstall(listener: Listener) {
        if (this.listener === listener) this.listener = null
    }

    /** Called from native code (JniHelper::CallStaticFunction); must not throw. */
    @JvmStatic
    fun onNativeSettingChanged(key: String, value: String) {
        runCatching {
            val spec = catalog[key] ?: return
            val valid = when (spec.kind) {
                SettingKind.ENUM -> value in spec.choices
                SettingKind.BOOLEAN -> value == "true" || value == "false"
                else -> false
            }
            if (valid) listener?.onSettingChanged(spec, value)
        }
    }
}
