package com.shadps4.android.runtime.settings

import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class ConsoleLanguageTest {
    private fun profile(value: JsonPrimitive) = RuntimeProfile(values = mapOf(ConsoleLanguage.ID to value))

    @Test fun chineseUsesOrbisIdsAndPerGameOverrides() {
        val simplified = profile(JsonPrimitive("简体中文"))
        assertEquals(11, ConsoleLanguage.resolve(simplified, RuntimeProfile()))
        assertEquals(10, ConsoleLanguage.resolve(simplified, profile(JsonPrimitive("繁體中文"))))
        assertEquals(11, ConsoleLanguage.resolve(simplified, RuntimeProfile()))
        assertEquals(1, ConsoleLanguage.resolve(RuntimeProfile(), RuntimeProfile()))
    }

    @Test fun numericConfigurationsAreNotAccepted() {
        for (numeric in listOf(JsonPrimitive(11), JsonPrimitive("10"))) {
            assertThrows(IllegalArgumentException::class.java) {
                ConsoleLanguage.resolve(profile(numeric), RuntimeProfile())
            }
            assertEquals(JsonPrimitive("English (United States)"), ConsoleLanguage.displayValue(numeric))
        }
    }

    @Test(expected = IllegalArgumentException::class)
    fun invalidStoredIdCannotReachNative() {
        ConsoleLanguage.resolve(profile(JsonPrimitive(99)), RuntimeProfile())
    }

    @Test fun invalidValueShowsNamedDefaultInEditor() {
        assertEquals(JsonPrimitive("English (United States)"), ConsoleLanguage.displayValue(JsonPrimitive(99)))
    }
}
