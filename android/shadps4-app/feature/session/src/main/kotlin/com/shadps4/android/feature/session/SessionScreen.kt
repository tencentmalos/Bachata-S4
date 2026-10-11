package com.shadps4.android.feature.session

import android.content.Intent
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.background
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.viewinterop.AndroidView
import androidx.activity.compose.BackHandler
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.findViewTreeLifecycleOwner
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.ManagedSessionState
import com.shadps4.android.runtime.session.RuntimeSurface
import com.shadps4.android.feature.session.controller.FixedControllerOverlay
import com.shadps4.android.feature.session.controller.TouchLayout
import com.shadps4.android.feature.session.controller.TouchLayoutRepository
import com.shadps4.android.feature.session.diagnostics.DiagnosticReportPhase
import com.shadps4.android.feature.session.diagnostics.DiagnosticReportSheet
import com.shadps4.android.feature.session.diagnostics.DiagnosticReportViewModel
import com.shadps4.android.feature.session.diagnostics.DiagnosticShare
import com.shadps4.android.data.RuntimeProfileStore
import com.shadps4.android.runtime.diagnostics.DiagnosticCheckpoint
import com.shadps4.android.runtime.diagnostics.DiagnosticReportContext
import com.shadps4.android.runtime.settings.ProfileScope
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.OutlinedButton
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.text.font.FontWeight
import com.shadps4.android.designsystem.theme.BachataPalette
import com.shadps4.android.runtime.input.ControllerSnapshot
import com.shadps4.android.runtime.input.Ps4Button
import com.shadps4.android.runtime.session.NativeFexSession
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.input.GamepadInputManager
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.ui.platform.LocalDensity

@Composable
fun SessionScreen(
    gameId: String,
    /** Stops the game and returns to the library, keeping the app alive. */
    onExit: () -> Unit = {},
    /** The screen shows both eyes side by side (a PSVR title in SBS): overlays go in each eye. */
    stereo: Boolean = false,
    viewModel: SessionViewModel = hiltViewModel(),
    diagnosticViewModel: DiagnosticReportViewModel = hiltViewModel(),
) {
    SessionWindowModeEffect()
    val context = LocalContext.current
    // The activity's lifecycle: the destination's own only resumes after the navigation transition.
    val lifecycle = LocalView.current.findViewTreeLifecycleOwner()?.lifecycle ?: LocalLifecycleOwner.current.lifecycle
    val dependencies = remember { EntryPointAccessors.fromApplication(context.applicationContext, TouchLayoutDependencies::class.java) }
    var touchLayout by remember { mutableStateOf(TouchLayout()) }
    val state by viewModel.state.collectAsState()
    val connectedControllers by GamepadInputManager.connectedControllers.collectAsState()
    val hasPhysicalController = connectedControllers.isNotEmpty()
    // Terminal publication follows native teardown. This renderer owns no guest
    // Surface and therefore also works when Prepare never produced a frame.
    LaunchedEffect(state) {
        val detail = when (val terminal = state) {
            is ManagedSessionState.Failed -> terminal.detail
            is ManagedSessionState.Stopped -> if (terminal.isUnexpected) terminal.toString() else null
            else -> null
        }
        if (detail != null && NativePad.nativeIsOpenXrConfigured()) {
            val token = NativeFexSession.nextXrErrorToken()
            var action = 0
            try {
                val error = withContext(Dispatchers.IO) { NativeFexSession.nativeShowXrError(token, detail) }
                if (error == null) {
                    while (action == 0) {
                        delay(50)
                        action = withContext(Dispatchers.IO) { NativeFexSession.nativePollXrErrorAction(token) }
                    }
                    if (action < 0) android.util.Log.e("XrError", NativeFexSession.nativeXrErrorStatus())
                } else {
                    android.util.Log.e("XrError", error)
                    // If OpenXR itself is unavailable, return to the ordinary
                    // Activity, which can display the retained terminal detail.
                    action = 2
                }
            } finally {
                withContext(NonCancellable + Dispatchers.IO) { NativeFexSession.nativeHideXrError(token) }
            }
            if (action == 1) viewModel.relaunch(gameId, lifecycle)
            else if (action != 0) onExit()
        }
    }

    val diagnosticState by diagnosticViewModel.uiState.collectAsState()

    val sessionUiPreferences = remember(context) {
        context.getSharedPreferences("session_ui", android.content.Context.MODE_PRIVATE)
    }
    var showTouchControls by remember(sessionUiPreferences) {
        mutableStateOf(sessionUiPreferences.getBoolean("show_touch_controls", true))
    }
    var showStopOverlay by remember { mutableStateOf(false) }
    var showSessionStopReport by remember { mutableStateOf(false) }
    var observedGeneration by rememberSaveable(gameId) { mutableStateOf(0L) }
    var notificationMessage by remember { mutableStateOf<String?>(null) }
    var notificationVisible by remember { mutableStateOf(false) }

    val saveDocumentLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument(DiagnosticShare.MIME_ZIP),
    ) { uri ->
        val zip = diagnosticState.zipFile
        if (uri != null && zip != null && zip.isFile) {
            runCatching {
                context.contentResolver.openOutputStream(uri)?.use { output ->
                    zip.inputStream().use { input -> input.copyTo(output) }
                }
            }
        }
    }

    LaunchedEffect(gameId) {
        dependencies.runtimeProfileStore().initializeControllerMappings(GamepadInputManager.connectedControllers.value)
        val global = dependencies.runtimeProfileStore().load(ProfileScope.Global)
        val game = dependencies.runtimeProfileStore().load(ProfileScope.Game(gameId))
        touchLayout = dependencies.touchLayoutRepository().load(game.touchLayoutId ?: global.touchLayoutId)
        com.shadps4.android.runtime.input.NativePadBridge.configureProfiles(
            game.controllerSlots.ifEmpty { global.controllerSlots },
        )
        if (!viewModel.launchOnEnter(gameId, lifecycle) && viewModel.state.value == ManagedSessionState.Idle) {
            // Restored after the app process died: the game this screen started is gone.
            android.util.Log.i("SessionLaunch", "Not restarting $gameId on a restored session screen")
            android.widget.Toast.makeText(context, "The game session has ended", android.widget.Toast.LENGTH_SHORT).show()
            onExit()
        }
        combine(
            dependencies.runtimeProfileStore().observe(ProfileScope.Global),
            dependencies.runtimeProfileStore().observe(ProfileScope.Game(gameId)),
        ) { globalProfile, gameProfile -> gameProfile.controllerSlots.ifEmpty { globalProfile.controllerSlots } }
            .collect { com.shadps4.android.runtime.input.NativePadBridge.configureProfiles(it) }
    }

    LaunchedEffect(state) {
        val current = state
        when (current) {
            is ManagedSessionState.Preparing -> observedGeneration = current.generation
            is ManagedSessionState.Ready -> observedGeneration = current.generation
            is ManagedSessionState.Running -> observedGeneration = current.generation
            is ManagedSessionState.Stopping -> observedGeneration = current.generation
            else -> Unit
        }
        // A normal Stop returns to the library only after native teardown publishes terminal.
        // Reports stay available from the library; don't strand a cancelled session on this page.
        if (current is ManagedSessionState.Stopped && current.generation == observedGeneration &&
            current.userRequestedStop && !current.isUnexpected) {
            showStopOverlay = false
            showSessionStopReport = false
            onExit()
            return@LaunchedEffect
        }
        val shouldOfferReport = when (current) {
            is ManagedSessionState.Stopped ->
                current.reportContext != null && current.isUnexpected
            is ManagedSessionState.Failed -> current.reportContext != null
            else -> false
        }
        showSessionStopReport = shouldOfferReport
        if (current != ManagedSessionState.Idle) {
            notificationMessage = current.label()
            notificationVisible = true
            delay(5000)
            notificationVisible = false
        }
    }

    val inputGeneration = when (val sessionState = state) {
        is ManagedSessionState.Preparing -> sessionState.generation
        is ManagedSessionState.Ready -> sessionState.generation
        is ManagedSessionState.Running -> sessionState.generation
        else -> 0L
    }
    var saveDialogVisible by remember(inputGeneration) { mutableStateOf(false) }
    SaveDataDialog(inputGeneration) { saveDialogVisible = it }
    val publishController = remember(inputGeneration) { ManagedSession.controllerPublisher(inputGeneration) }
    DisposableEffect(inputGeneration, showStopOverlay, saveDialogVisible) {
        com.shadps4.android.runtime.input.NativePadBridge.setUiCaptured(inputGeneration,showStopOverlay || saveDialogVisible)
        onDispose { com.shadps4.android.runtime.input.NativePadBridge.setUiCaptured(inputGeneration,false) }
    }

    BackHandler(
        enabled = state is ManagedSessionState.Running ||
            state is ManagedSessionState.Ready ||
            state is ManagedSessionState.Stopping,
    ) {
        showStopOverlay = !showStopOverlay
    }

    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val topOffset = maxHeight * 0.3f
        val topOffsetPx = with(density) { topOffset.toPx() }

        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { currentContext ->
                SurfaceView(currentContext).also { view ->
                    view.setOnTouchListener { target, event ->
                        val bridge = com.shadps4.android.runtime.input.NativePadBridge
                        if (target.width <= 0 || target.height <= 0) false else {
                            when (event.actionMasked) {
                                android.view.MotionEvent.ACTION_DOWN, android.view.MotionEvent.ACTION_POINTER_DOWN,
                                android.view.MotionEvent.ACTION_UP, android.view.MotionEvent.ACTION_POINTER_UP -> {
                                    val i = event.actionIndex
                                    val phase = if (event.actionMasked == android.view.MotionEvent.ACTION_DOWN || event.actionMasked == android.view.MotionEvent.ACTION_POINTER_DOWN) 0 else 2
                                    bridge.overlayPointer(event.getPointerId(i), phase, event.getX(i)/target.width, event.getY(i)/target.height)
                                }
                                android.view.MotionEvent.ACTION_MOVE -> {
                                    for (i in 0 until event.pointerCount) bridge.overlayPointer(event.getPointerId(i), 1, event.getX(i)/target.width, event.getY(i)/target.height)
                                }
                                android.view.MotionEvent.ACTION_CANCEL -> bridge.cancelOverlayPointers()
                            }
                            true
                        }
                    }
                    view.holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) = Unit
                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                            com.shadps4.android.runtime.input.NativePadBridge.updateOverlayDensity(view.resources.displayMetrics.density)
                            if (width > 0 && height > 0) {
                                ManagedSession.attachSurface(RuntimeSurface(holder.surface, width, height))
                            }
                        }
                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            com.shadps4.android.runtime.input.NativePadBridge.cancelOverlayPointers()
                            ManagedSession.detachSurface(holder.surface)
                        }
                    })
                }
            },
        )

        // Removing the View stops drawing AND hit testing; its disposal sends
        // only the overlay neutral state. Physical/debugbus sources stay independent.
        if (showTouchControls && !hasPhysicalController) FixedControllerOverlay(
            layout = touchLayout,
            onOverlayPointer = com.shadps4.android.runtime.input.NativePadBridge::overlayPointer,
            onOverlayCancel = com.shadps4.android.runtime.input.NativePadBridge::cancelOverlayPointers,
            onSnapshot = { snapshot ->
                if ((snapshot.buttons and Ps4Button.PS) != 0L) {
                    showStopOverlay = true
                }
                if (showStopOverlay) {
                    publishController(ControllerSnapshot.Neutral)
                } else {
                    // Host InputHub merges touch and physical sources independently.
                    publishController(snapshot)
                }
            }
        )

        // Notification pill sliding down from top, staying 5s, vanishing moving down
        PerEye(stereo, Alignment.TopCenter) {
            AnimatedVisibility(
                visible = notificationVisible && notificationMessage != null,
                enter = slideInVertically(initialOffsetY = { -it - topOffsetPx.toInt() }) + fadeIn(),
                exit = slideOutVertically(targetOffsetY = { it * 3 }) + fadeOut(),
                modifier = Modifier.padding(top = topOffset)
            ) {
                notificationMessage?.let { msg ->
                    NotificationPill(message = msg)
                }
            }
        }

        if (showSessionStopReport) {
            SessionStopReportOverlay(
                state = state,
                onCreateReport = {
                    reportContextFromState(state)?.let { diagnosticViewModel.openReview(it) }
                },
                onRetry = {
                    showSessionStopReport = false
                    viewModel.relaunch(gameId, lifecycle)
                },
                onClose = {
                    showSessionStopReport = false
                    onExit()
                },
            )
        }

        if (
            diagnosticState.phase != DiagnosticReportPhase.IDLE &&
            diagnosticState.phase != DiagnosticReportPhase.CANCELLED &&
            diagnosticState.context != null
        ) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.8f)),
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    modifier = Modifier
                        .padding(16.dp)
                        .background(BachataPalette.Surface, RoundedCornerShape(16.dp))
                        .border(1.dp, BachataPalette.Secondary.copy(alpha = 0.3f), RoundedCornerShape(16.dp)),
                ) {
                    DiagnosticReportSheet(
                        state = diagnosticState,
                        onDescriptionChange = diagnosticViewModel::setUserDescription,
                        onScreenshotToggle = diagnosticViewModel::setIncludeScreenshot,
                        onCreateAndShare = {
                            diagnosticViewModel.createAndShare { file ->
                                context.startActivity(
                                    DiagnosticShare.createChooser(context, file, diagnosticState.context?.reportId.orEmpty()),
                                )
                            }
                        },
                        onSaveLocally = {
                            diagnosticViewModel.createAndSave { file ->
                                saveDocumentLauncher.launch(file.name)
                            }
                        },
                        onCancel = {
                            diagnosticViewModel.dismiss()
                        },
                        onRetry = {
                            reportContextFromState(state)?.let { diagnosticViewModel.openReview(it) }
                        },
                    )
                }
            }
        }

        if (showStopOverlay) {
            SessionMenu(
                showTouchOption = !hasPhysicalController,
                showTouchControls = showTouchControls,
                onToggleTouch = {
                    showTouchControls = !showTouchControls
                    sessionUiPreferences.edit().putBoolean("show_touch_controls", showTouchControls).apply()
                },
                onOverlayControls = {
                    showStopOverlay = false
                    com.shadps4.android.runtime.input.NativePadBridge.openOverlayControls()
                },
                onResume = { showStopOverlay = false },
                onStop = {
                    context.startService(
                        Intent(ManagedSession.ACTION_STOP).setClassName(context.packageName, ManagedSession.SERVICE_CLASS),
                    )
                    showStopOverlay = false
                },
            )
        }
    }
}

/** `content` at `alignment` of the screen, or of each half when the screen shows two eyes. */
@Composable
private fun androidx.compose.foundation.layout.BoxScope.PerEye(
    stereo: Boolean,
    alignment: Alignment,
    content: @Composable androidx.compose.foundation.layout.BoxScope.() -> Unit,
) {
    if (!stereo) {
        Box(Modifier.align(alignment), content = content)
        return
    }
    Row(Modifier.matchParentSize()) {
        repeat(2) {
            Box(
                Modifier.weight(1f).fillMaxSize(),
                contentAlignment = alignment,
                content = content,
            )
        }
    }
}

@Composable
fun NotificationPill(message: String) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(24.dp))
            .background(
                Brush.horizontalGradient(
                    colors = listOf(
                        Color(0xFF26262B),
                        Color(0xFF1C1C20)
                    )
                )
            )
            .border(
                1.dp,
                Brush.horizontalGradient(
                    colors = listOf(
                        BachataPalette.Accent,
                        BachataPalette.Secondary
                    )
                ),
                RoundedCornerShape(24.dp)
            )
            .padding(horizontal = 24.dp, vertical = 12.dp)
    ) {
        Text(
            text = message,
            color = BachataPalette.Primary,
            style = androidx.compose.material3.MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.SemiBold
        )
    }
}

@EntryPoint
@InstallIn(SingletonComponent::class)
interface TouchLayoutDependencies {
    fun runtimeProfileStore(): RuntimeProfileStore
    fun touchLayoutRepository(): TouchLayoutRepository
}

private fun ManagedSessionState.label(): String = when (this) {
    ManagedSessionState.Idle -> "Idle"
    is ManagedSessionState.Preparing -> "Preparing: $stage"
    is ManagedSessionState.Ready -> "Ready: $gameId"
    is ManagedSessionState.Running -> "Running: $gameId"
    is ManagedSessionState.Stopping -> "Stopping: $gameId"
    is ManagedSessionState.Failed -> "Error: $detail"
    is ManagedSessionState.Stopped -> when {
        userRequestedStop -> "Manually exited"
        else -> "Stopped: ${exitCode ?: "unknown"}"
    }
}

private fun reportContextFromState(state: ManagedSessionState): DiagnosticReportContext? = when (state) {
    is ManagedSessionState.Stopped -> state.reportContext
    is ManagedSessionState.Failed -> state.reportContext
    else -> null
}

@Composable
private fun SessionStopReportOverlay(
    state: ManagedSessionState,
    onCreateReport: () -> Unit,
    onRetry: () -> Unit,
    onClose: () -> Unit,
) {
    val reportContext = reportContextFromState(state)
    val userRequestedStop = when (state) {
        is ManagedSessionState.Stopped -> state.userRequestedStop
        else -> reportContext?.userRequestedStop == true
    }
    val exitCode = when (state) {
        is ManagedSessionState.Stopped -> state.exitCode
        else -> reportContext?.termination?.exitCode
    }
    val lastStage = reportContext?.lastCheckpoint?.let { name ->
        runCatching { DiagnosticCheckpoint.valueOf(name).displayLabel() }.getOrDefault(name)
    } ?: "unknown"
    val reportId = reportContext?.reportId ?: "unavailable"
    val termination = reportContext?.termination
    val processLine = when {
        userRequestedStop ->
            "Process: manually stopped" +
                (termination?.exitCode?.let { " (exit value $it)" } ?: "")
        termination?.signalNumber != null ->
            "Process: ${termination.processRole.name.lowercase()} · Signal ${termination.signalNumber}" +
                (termination.signalName?.let { " ($it)" } ?: "")
        termination != null ->
            "Process reason: Not available (exit value ${termination.exitCode ?: "unknown"})"
        else -> "Process reason: Not available"
    }
    val title = if (userRequestedStop) {
        "Game was manually exited"
    } else {
        "Game stopped unexpectedly"
    }
    val subtitle = if (userRequestedStop) {
        "You stopped the session. You can still create a diagnostic report if something looked wrong."
    } else {
        "The emulation process ended before a normal shutdown."
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.8f)),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier
                .padding(20.dp)
                .background(BachataPalette.Surface, RoundedCornerShape(16.dp))
                .border(1.dp, BachataPalette.Secondary.copy(alpha = 0.3f), RoundedCornerShape(16.dp))
                .padding(24.dp),
        ) {
            Text(
                text = title,
                color = BachataPalette.Primary,
                fontWeight = FontWeight.Bold,
                style = androidx.compose.material3.MaterialTheme.typography.headlineSmall,
            )
            Text(
                text = subtitle,
                color = BachataPalette.Secondary,
                modifier = Modifier.padding(top = 8.dp, bottom = 16.dp),
            )
            Text("Exit value: ${exitCode ?: "unknown"}", color = BachataPalette.Primary)
            Text("Last completed stage: $lastStage", color = BachataPalette.Primary)
            Text(processLine, color = BachataPalette.Secondary, modifier = Modifier.padding(top = 4.dp))
            Text("Report ID: $reportId", color = BachataPalette.Secondary, modifier = Modifier.padding(bottom = 16.dp))
            Row {
                if (reportContext != null) {
                    Button(onClick = onCreateReport, modifier = Modifier.padding(end = 8.dp)) {
                        Text("Create diagnostic report")
                    }
                }
                OutlinedButton(onClick = onRetry, modifier = Modifier.padding(end = 8.dp)) {
                    Text("Retry")
                }
                OutlinedButton(onClick = onClose) {
                    Text("Close")
                }
            }
        }
    }
}
