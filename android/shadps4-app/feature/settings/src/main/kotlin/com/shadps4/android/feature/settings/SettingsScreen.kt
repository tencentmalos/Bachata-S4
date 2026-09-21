package com.shadps4.android.feature.settings

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import com.shadps4.android.designsystem.BachataPanel
import com.shadps4.android.designsystem.BachataScreenHeader
import com.shadps4.android.designsystem.theme.BachataPalette
import com.shadps4.android.feature.settings.input.ControllerMappingScreen
import com.shadps4.android.feature.settings.input.TouchLayoutEditorScreen
import com.shadps4.android.runtime.input.GamepadInputManager
import com.shadps4.android.runtime.settings.ConsoleLanguage
import com.shadps4.android.runtime.settings.ProfileScope
import kotlinx.serialization.json.JsonPrimitive
import com.shadps4.android.runtime.settings.SettingKind

private enum class SettingsPage(val title: String) {
    System("System"), Graphics("Graphics"), Controllers("Controller Buttons"), Touch("Touch Layout"),
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun SettingsScreen(
    onBack: () -> Unit,
    initialGameId: String? = null,
    viewModel: SettingsViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsState()
    var page by remember { mutableStateOf<SettingsPage?>(null) }
    var focused by remember(page) { mutableIntStateOf(0) }
    val pageSettings = state.settings.filter { if (page == SettingsPage.System) it.category == "System" else it.category == "GPU" }
    LaunchedEffect(initialGameId) {
        viewModel.selectScope(initialGameId?.let(ProfileScope::Game) ?: ProfileScope.Global)
    }
    val back: () -> Unit = { if (page == null) onBack() else page = null }
    BackHandler(onBack = back)

    // Controller mapping owns its capture/navigation listener while open.
    if (page != SettingsPage.Controllers) {
        DisposableEffect(page, focused, state) {
            GamepadInputManager.registerNavListener { event ->
                if (!event.pressed) return@registerNavListener false
                when {
                    event.control == "circle" -> { back(); true }
                    page == null -> {
                        when (event.control) {
                            "dpad_up" -> focused = (focused - 1).coerceAtLeast(0)
                            "dpad_down" -> focused = (focused + 1).coerceAtMost(SettingsPage.entries.lastIndex)
                            "cross" -> page = SettingsPage.entries[focused]
                            else -> return@registerNavListener false
                        }
                        true
                    }
                    page == SettingsPage.Graphics || page == SettingsPage.System -> {
                        when (event.control) {
                            "dpad_up" -> focused = (focused - 1).coerceAtLeast(0)
                            "dpad_down" -> focused = (focused + 1).coerceAtMost(pageSettings.lastIndex)
                            "dpad_left", "dpad_right", "cross" -> {
                                val spec = pageSettings[focused]
                                val current = (state.effectiveValue(spec) as? JsonPrimitive)?.content
                                if (spec.kind == SettingKind.BOOLEAN) {
                                    viewModel.setValue(spec, JsonPrimitive(current != "true"))
                                } else {
                                    val offset = if (event.control == "dpad_left") -1 else 1
                                    val index = Math.floorMod(spec.choices.indexOf(current) + offset, spec.choices.size)
                                    viewModel.setText(spec, spec.choices[index])
                                }
                            }
                            "triangle" -> viewModel.setValue(pageSettings[focused], null)
                            else -> return@registerNavListener false
                        }
                        true
                    }
                    else -> false
                }
            }
            onDispose { GamepadInputManager.unregisterNavListener() }
        }
    }

    Scaffold(containerColor = BachataPalette.Canvas) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            BachataScreenHeader(
                title = page?.title ?: if (initialGameId == null) "Settings" else "Game Settings · $initialGameId",
                onBack = back,
            )
            when (page) {
                null -> Column(
                    modifier = Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    SettingsPage.entries.forEachIndexed { index, destination ->
                        Surface(
                            onClick = { page = destination },
                            color = BachataPalette.Surface,
                            border = BorderStroke(1.dp, if (focused == index) BachataPalette.Accent else Color.Transparent),
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text(destination.title, Modifier.padding(20.dp), color = BachataPalette.Primary)
                        }
                    }
                }
                SettingsPage.System, SettingsPage.Graphics -> Column(
                    modifier = Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text("Changes apply next game launch.", color = BachataPalette.Secondary)
                    pageSettings.forEachIndexed { index, spec ->
                        val current = (state.effectiveValue(spec) as? JsonPrimitive)?.content
                        BachataPanel(modifier = Modifier.fillMaxWidth()) {
                            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text(spec.title, style = MaterialTheme.typography.titleMedium,
                                    color = if (focused == index) BachataPalette.Accent else BachataPalette.Primary)
                                Text(spec.help, color = BachataPalette.Secondary)
                                if (spec.id == ConsoleLanguage.ID) {
                                    var expanded by remember { mutableStateOf(false) }
                                    Box {
                                        TextButton(onClick = { focused = index; expanded = true }) {
                                            Text(current ?: "Choose language")
                                        }
                                        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false },
                                            modifier = Modifier.heightIn(max = 360.dp)) {
                                            spec.choices.forEach { choice ->
                                                DropdownMenuItem(text = { Text(choice) }, onClick = {
                                                    viewModel.setText(spec, choice)
                                                    expanded = false
                                                })
                                            }
                                        }
                                    }
                                } else FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                    val choices = if (spec.kind == SettingKind.BOOLEAN) listOf("false", "true") else spec.choices
                                    choices.forEach { choice ->
                                        Surface(
                                            onClick = {
                                                focused = index
                                                if (spec.kind == SettingKind.BOOLEAN) viewModel.setValue(spec, JsonPrimitive(choice == "true"))
                                                else viewModel.setText(spec, choice)
                                            },
                                            color = if (choice == current) BachataPalette.Accent else BachataPalette.RaisedSurface,
                                            shape = MaterialTheme.shapes.small,
                                        ) {
                                            Text(if (spec.kind == SettingKind.BOOLEAN) { if (choice == "true") "On" else "Off" } else choice, Modifier.padding(12.dp),
                                                color = if (choice == current) BachataPalette.OnAccent else BachataPalette.Primary)
                                        }
                                    }
                                }
                                val game = state.scope is ProfileScope.Game
                                if (game && spec.id !in state.profile.values) {
                                    Text("Using global setting", color = BachataPalette.Secondary)
                                }
                                TextButton(onClick = { viewModel.setValue(spec, null) }) {
                                    Text(if (game) "Use global setting" else "Reset to default")
                                }
                            }
                        }
                    }
                    state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                }
                SettingsPage.Controllers -> ControllerMappingScreen(scope = state.scope, onBack = back)
                SettingsPage.Touch -> TouchLayoutEditorScreen(scope = state.scope, onBack = back)
            }
        }
    }
}
