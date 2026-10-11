package com.shadps4.android.feature.session

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.input.key.*
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.shadps4.android.designsystem.theme.BachataPalette
import com.shadps4.android.runtime.input.GamepadInputManager
import com.shadps4.android.runtime.input.NavControllerEvent

private data class MenuAction(val id: String, val label: String, val run: () -> Unit)

@Composable
@OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)
fun SessionMenu(
    showTouchOption: Boolean,
    showTouchControls: Boolean,
    onToggleTouch: () -> Unit,
    onOverlayControls: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
) {
    val actions = buildList {
        if (showTouchOption) add(MenuAction("touch", "Touch controls: ${if (showTouchControls) "Shown" else "Hidden"}", onToggleTouch))
        add(MenuAction("overlay", "Status overlay controls", onOverlayControls))
        add(MenuAction("resume", "Resume", onResume))
        add(MenuAction("stop", "Stop", onStop))
    }
    val latestActions by rememberUpdatedState(actions)
    val latestResume by rememberUpdatedState(onResume)
    val ids = actions.map { it.id }
    val requesters = remember(ids) { ids.associateWith { FocusRequester() } }
    val inputMode = LocalInputModeManager.current
    var selected by remember { mutableStateOf("resume") }
    val input = remember { SessionMenuInput() }
    val navigate: (SessionMenuInput.Command) -> Unit = { command ->
        val current = latestActions.indexOfFirst { it.id == selected }.coerceAtLeast(0)
        when (command) {
            SessionMenuInput.Command.Dismiss -> latestResume()
            SessionMenuInput.Command.Activate -> latestActions[current].run()
            else -> {
                val next = (current + if (command == SessionMenuInput.Command.Previous) -1 else 1)
                    .coerceIn(latestActions.indices)
                selected = latestActions[next].id
                requesters.getValue(selected).requestFocus()
            }
        }
    }
    val latestNavigate by rememberUpdatedState(navigate)
    DisposableEffect(input) {
        val listener: (NavControllerEvent) -> Boolean = { event ->
            input.accept(event)?.let(latestNavigate)
            true // Modal: consume releases and unrelated game buttons as well.
        }
        GamepadInputManager.registerNavListener(listener)
        onDispose { GamepadInputManager.unregisterNavListener(listener) }
    }
    LaunchedEffect(ids) {
        // This panel is commonly opened by a touch swipe. Give it real focus immediately,
        // so the first direction press navigates instead of merely leaving touch mode.
        inputMode.requestInputMode(InputMode.Keyboard)
        if (selected !in ids) selected = "resume"
        requesters.getValue(selected).requestFocus()
    }
    BackHandler(onBack = onResume)
    Box(
        Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.75f))
            .clickable(onClick = onResume),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.padding(16.dp).widthIn(max = 420.dp)
                .background(BachataPalette.Surface, RoundedCornerShape(16.dp))
                .border(1.dp, BachataPalette.Secondary.copy(alpha = 0.3f), RoundedCornerShape(16.dp))
                .clickable(enabled = false) {}
                .onPreviewKeyEvent { event ->
                    val command = when (event.key) {
                        Key.DirectionUp, Key.DirectionLeft -> SessionMenuInput.Command.Previous
                        Key.DirectionDown, Key.DirectionRight -> SessionMenuInput.Command.Next
                        Key.Enter, Key.NumPadEnter, Key.Spacebar -> SessionMenuInput.Command.Activate
                        Key.Escape -> SessionMenuInput.Command.Dismiss
                        else -> null
                    }
                    if (command == null) false else {
                        if (event.type == KeyEventType.KeyDown) latestNavigate(command)
                        true
                    }
                }
                .verticalScroll(rememberScrollState()).padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Emulation Interrupted", color = BachataPalette.Primary,
                style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.Bold)
            Text("Do you want to stop the current session?", color = BachataPalette.Secondary,
                modifier = Modifier.padding(bottom = 8.dp))
            actions.forEach { action ->
                Button(
                    onClick = action.run,
                    modifier = Modifier.fillMaxWidth().focusRequester(requesters.getValue(action.id))
                        .onFocusChanged { if (it.isFocused) selected = action.id }
                        .border(if (selected == action.id) 2.dp else 0.dp,
                            if (selected == action.id) BachataPalette.Accent else Color.Transparent,
                            RoundedCornerShape(24.dp)),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (action.id == "stop") Color(0xFFD32F2F) else BachataPalette.RaisedSurface,
                        contentColor = BachataPalette.Primary,
                    ),
                ) { Text(action.label) }
            }
        }
    }
}
