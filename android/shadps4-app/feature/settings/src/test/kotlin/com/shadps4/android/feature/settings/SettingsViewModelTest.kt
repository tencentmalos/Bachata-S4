package com.shadps4.android.feature.settings

import com.shadps4.android.data.RuntimeProfileStore
import com.shadps4.android.runtime.settings.ProfileScope
import com.shadps4.android.runtime.settings.ConsoleLanguage
import com.shadps4.android.runtime.settings.InternalScale
import com.shadps4.android.runtime.settings.RuntimeProfile
import com.shadps4.android.runtime.settings.RuntimeSettingCatalog
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Before
import org.junit.Test
import org.junit.rules.TemporaryFolder

@OptIn(ExperimentalCoroutinesApi::class)
class SettingsViewModelTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val dispatcher = StandardTestDispatcher()

    @Before
    fun setUp() {
        Dispatchers.setMain(dispatcher)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private fun viewModel() = SettingsViewModel(RuntimeProfileStore(temporaryFolder.root))

    @Test
    fun languageEditorFeedsNamedSelectionToNativeAndInheritsGlobal() = runTest(dispatcher) {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(values = mapOf(ConsoleLanguage.ID to JsonPrimitive("繁體中文"))) }
        val model = SettingsViewModel(store)
        val scope = ProfileScope.Game("CUSA03023")
        model.selectScope(scope)
        advanceUntilIdle()
        val spec = model.state.value.settings.single { it.id == ConsoleLanguage.ID }
        assertEquals(JsonPrimitive("繁體中文"), model.state.value.effectiveValue(spec))
        model.setText(spec, "简体中文")
        advanceUntilIdle()
        assertEquals(11, ConsoleLanguage.resolve(store.load(ProfileScope.Global), store.load(scope)))
        assertEquals(JsonPrimitive("简体中文"), model.state.value.effectiveValue(spec))
        model.setValue(spec, null)
        advanceUntilIdle()
        assertEquals(10, ConsoleLanguage.resolve(store.load(ProfileScope.Global), store.load(scope)))
        assertEquals(JsonPrimitive("繁體中文"), store.load(ProfileScope.Global).values[ConsoleLanguage.ID])
    }

    @Test
    fun rejectsLegacySettingsAndInvalidChoicesWithoutLosingStoredData() = runTest(dispatcher) {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val legacy = RuntimeSettingCatalog.loadFromResources().shadPs4.single { it.id == "gpu.direct_memory_access_enabled" }
        store.update(ProfileScope.Global) { it.copy(values = mapOf(legacy.id to JsonPrimitive(true))) }
        val model = SettingsViewModel(store)
        advanceUntilIdle()
        assertFalse(model.state.value.settings.any { it.id == legacy.id })
        model.setValue(legacy, JsonPrimitive(false))
        advanceUntilIdle()
        assertEquals(JsonPrimitive(true), store.load(ProfileScope.Global).values[legacy.id])
        val scale = model.state.value.settings.single { it.id == InternalScale.ID }
        model.setText(scale, "0.2")
        advanceUntilIdle()
        assertEquals(null, store.load(ProfileScope.Global).values[scale.id])
        model.setText(scale, "0.75")
        advanceUntilIdle()
        assertEquals(JsonPrimitive(true), store.load(ProfileScope.Global).values[legacy.id])
    }

    @Test
    fun gameEditorShowsActualInheritedValueAndUpdatesAfterReset() = runTest(dispatcher) {
        val store = RuntimeProfileStore(temporaryFolder.root)
        store.update(ProfileScope.Global) { it.copy(values = mapOf(InternalScale.ID to JsonPrimitive("0.75"))) }
        val model = SettingsViewModel(store)
        model.selectScope(ProfileScope.Game("CUSA50828"))
        advanceUntilIdle()
        val spec = model.state.value.settings.single { it.id == InternalScale.ID }
        assertEquals(JsonPrimitive("0.75"), model.state.value.effectiveValue(spec))
        model.setText(spec, "1.0")
        advanceUntilIdle()
        assertEquals(JsonPrimitive("1.0"), model.state.value.effectiveValue(spec))
        model.setValue(spec, null)
        advanceUntilIdle()
        assertEquals(JsonPrimitive("0.75"), model.state.value.effectiveValue(spec))
        store.update(ProfileScope.Global) { it.copy(values = it.values + (InternalScale.ID to JsonPrimitive("0.5"))) }
        advanceUntilIdle()
        assertEquals(JsonPrimitive("0.5"), model.state.value.effectiveValue(spec))
    }

    @Test
    fun internalScaleIsEditableInGpuSettingsAndPersistsAcrossScopes() = runTest(dispatcher) {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = SettingsViewModel(store)
        advanceUntilIdle()
        val spec = viewModel.state.value.settings.single { it.id == InternalScale.ID }
        assertEquals(listOf("0.25", "0.375", "0.5", "0.75", "1.0"), spec.choices)
        assertEquals(JsonPrimitive("0.5"), spec.defaultValue)
        assertTrue(spec.restartRequired)
        assertEquals(null, spec.readOnlyReason)

        for ((choice, percent) in listOf("0.25" to 25f, "0.375" to 37.5f, "0.5" to 50f, "0.75" to 75f, "1.0" to 100f)) {
            viewModel.setText(spec, choice)
            advanceUntilIdle()
            val global = store.load(ProfileScope.Global)
            assertEquals(JsonPrimitive(choice), global.values[spec.id])
            assertEquals(percent, InternalScale.resolve(global, RuntimeProfile()))
        }
        val gameScope = ProfileScope.Game("CUSA50828")
        viewModel.selectScope(gameScope)
        advanceUntilIdle()
        viewModel.setText(spec, "0.75")
        advanceUntilIdle()
        assertEquals(75f, InternalScale.resolve(store.load(ProfileScope.Global), store.load(gameScope)))
        viewModel.setValue(spec, null)
        advanceUntilIdle()
        assertEquals(100f, InternalScale.resolve(store.load(ProfileScope.Global), store.load(gameScope)))

        viewModel.selectScope(ProfileScope.Global)
        advanceUntilIdle()
        viewModel.setValue(spec, null)
        advanceUntilIdle()
        assertEquals(50f, InternalScale.resolve(store.load(ProfileScope.Global), store.load(gameScope)))
    }
    @Test
    fun booleanMsaaEditorPersistsAndResetsGameOverride() = runTest(dispatcher) {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val model = SettingsViewModel(store)
        advanceUntilIdle()
        val id = com.shadps4.android.runtime.settings.ForceDisableMsaa.ID
        val spec = model.state.value.settings.single { it.id == id }
        model.setValue(spec, JsonPrimitive(true))
        advanceUntilIdle()
        assertEquals(JsonPrimitive(true), store.load(ProfileScope.Global).values[id])
        val game = ProfileScope.Game("CUSA12878")
        model.selectScope(game)
        advanceUntilIdle()
        model.setValue(spec, JsonPrimitive(false))
        advanceUntilIdle()
        assertEquals(JsonPrimitive(false), model.state.value.effectiveValue(spec))
        model.setText(spec, "broken")
        advanceUntilIdle()
        assertTrue(model.state.value.error != null)
        assertEquals(JsonPrimitive(false), store.load(game).values[id])
        model.setValue(spec, null)
        advanceUntilIdle()
        assertEquals(JsonPrimitive(true), model.state.value.effectiveValue(spec))
    }

}
