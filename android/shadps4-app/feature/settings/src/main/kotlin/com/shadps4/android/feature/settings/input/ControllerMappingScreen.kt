package com.shadps4.android.feature.settings.input

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import com.shadps4.android.designsystem.BachataBanner
import com.shadps4.android.designsystem.BachataPrimaryButton
import com.shadps4.android.designsystem.theme.BachataPalette
import com.shadps4.android.runtime.input.GamepadInputManager
import com.shadps4.android.runtime.settings.ProfileScope

// Diagram zones in gamepad-nav traversal order.
private val DIAGRAM_CONTROLS = listOf(
    "l2", "l1", "r1", "r2",
    "l3", "dpad_up", "dpad_left", "dpad_right", "dpad_down",
    "share", "ps", "touchpad", "options",
    "triangle", "square", "circle", "cross", "r3",
)

@Composable
fun ControllerMappingScreen(
    scope: ProfileScope,
    onBack: () -> Unit,
    viewModel: ControllerMappingViewModel = hiltViewModel(),
) {
    LaunchedEffect(scope) { viewModel.load(scope) }
    val state by viewModel.state.collectAsState()
    val profile = state.profiles[state.slot]

    var focusedControl by remember { mutableStateOf<String?>(null) }

    // Gamepad navigation: D-pad moves focus across diagram zones, cross captures, circle backs out.
    DisposableEffect(state.slot) {
        GamepadInputManager.registerNavListener { event ->
            if (!event.pressed) return@registerNavListener false
            when (event.control) {
                "dpad_right" -> {
                    val idx = DIAGRAM_CONTROLS.indexOf(focusedControl)
                    focusedControl = DIAGRAM_CONTROLS[(idx + 1).coerceIn(0, DIAGRAM_CONTROLS.lastIndex)]
                    true
                }
                "dpad_left" -> {
                    val idx = DIAGRAM_CONTROLS.indexOf(focusedControl)
                    focusedControl = DIAGRAM_CONTROLS[(idx - 1).coerceIn(0, DIAGRAM_CONTROLS.lastIndex)]
                    true
                }
                "cross" -> {
                    focusedControl?.let { viewModel.capture(it) }
                    true
                }
                "circle" -> { onBack(); true }
                else -> false
            }
        }
        onDispose { viewModel.cancelCapture(); GamepadInputManager.unregisterNavListener() }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 4.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // --- Action bar ---
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            BachataPrimaryButton(onClick = { viewModel.autoMap(useHatDpad = true) }, modifier = Modifier.weight(1f)) {
                Text("Reset Buttons")
            }
            BachataPrimaryButton(onClick = { viewModel.captureSequential() }, modifier = Modifier.weight(1f)) {
                Text("Map All Buttons")
            }
            TextButton(onClick = { viewModel.clear() }) { Text("Clear") }
        }

        // --- Capture banner ---
        val captureTarget = state.captureQueue.firstOrNull()
        if (captureTarget != null) {
            val total = state.captureQueue.size
            BachataBanner(
                title = "Press input for: $captureTarget",
                subtitle = if (total > 1) "${total} remaining · Press a button or D-pad direction" else "Press a button or D-pad direction",
                containerColor = BachataPalette.Info,
                titleColor = BachataPalette.InfoText,
                subtitleColor = BachataPalette.InfoSubtle,
                actions = { TextButton(onClick = { viewModel.cancelCapture() }) { Text("Cancel") } },
            )
        }

        // --- Conflict banner ---
        val conflict = state.conflict
        if (conflict != null) {
            BachataBanner(
                title = "Input already maps ${conflict.existing}",
                subtitle = "Replace ${conflict.existing} with ${conflict.target}?",
                containerColor = BachataPalette.Warning,
                titleColor = BachataPalette.WarningText,
                subtitleColor = BachataPalette.WarningText,
                actions = {
                    BachataPrimaryButton(onClick = { viewModel.replaceConflict() }) { Text("Replace") }
                    TextButton(onClick = { viewModel.cancelConflict() }) { Text("Cancel") }
                },
            )
        }

        Row(
            modifier = Modifier.fillMaxWidth().clickable { viewModel.setSwapFaceButtons(!profile.swapFaceButtons) },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Checkbox(checked = profile.swapFaceButtons, onCheckedChange = viewModel::setSwapFaceButtons)
            Column {
                Text("Xbox / Nintendo 面键翻转", color = BachataPalette.Primary)
                Text("交换 A/B 与 X/Y；不影响触屏按键", color = BachataPalette.Secondary)
            }
        }

        // --- Visual gamepad diagram ---
        GamepadDiagram(
            profile = profile,
            capturingControl = captureTarget,
            focusedControl = focusedControl,
            onCapture = { viewModel.capture(it) },
        )

        state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        if (scope is ProfileScope.Game) {
            TextButton(onClick = viewModel::inherit) { Text("Use global settings") }
        }
        Text("Controller 1 · Changes apply next game launch.", color = BachataPalette.Secondary)
    }
}
