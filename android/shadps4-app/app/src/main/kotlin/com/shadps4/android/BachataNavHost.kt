package com.shadps4.android

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.foundation.layout.Column
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.shadps4.android.data.GameRepository
import com.shadps4.android.data.GameInstallVerifier
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.ManagedSessionState
import com.shadps4.android.feature.library.LibraryScreen
import com.shadps4.android.feature.settings.SettingsScreen
import com.shadps4.android.feature.setup.SetupScreen
import com.shadps4.android.feature.session.SessionScreen
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

object BachataRoutes {
    const val Setup = "setup"
    const val Library = "library"
    const val Game = "game/{id}"
    const val Session = "session/{id}"
    const val Settings = "settings"
    const val GameSettings = "settings/game/{id}"
    fun gameSettings(id: String) = "settings/game/$id"
}

@EntryPoint
@InstallIn(SingletonComponent::class)
interface BachataNavEntryPoint {
    fun gameRepository(): GameRepository
    fun runtimeProfiles(): com.shadps4.android.data.RuntimeProfileStore
}

@Composable
fun BachataNavHost(startDestination: String = BachataRoutes.Setup, openLastGameRequest: Int = 0,
                   requestedGameId: String? = null, onGameRequestConsumed: () -> Unit = {},
                   openXrEnabled: Boolean = false,
                   switchDisplayActivity: (String, Boolean) -> Unit = { _, _ -> },
                   exitXrToLibrary: () -> Unit = {}) {
    val navController = rememberNavController()
    val context = LocalContext.current
    val graph = remember {
        EntryPointAccessors.fromApplication(
            context.applicationContext,
            BachataNavEntryPoint::class.java,
        )
    }

    LaunchedEffect(requestedGameId) {
        val id = requestedGameId ?: return@LaunchedEffect
        // This is only an identifier. The repository and installed-content gate below
        // validate it before any session is created (including exported Activity intents).
        val game = withContext(Dispatchers.IO) { graph.gameRepository().getGame(id) }
        if (game != null && !sessionBusy()) navController.navigate("session/${android.net.Uri.encode(game.id)}") {
            popUpTo(BachataRoutes.Library)
            launchSingleTop = true
        }
        onGameRequestConsumed()
    }

    LaunchedEffect(openLastGameRequest) {
        if (openLastGameRequest == 0) return@LaunchedEffect
        fun busy(): Boolean = when (ManagedSession.state.value) {
            is ManagedSessionState.Preparing, is ManagedSessionState.Ready,
            is ManagedSessionState.Running, is ManagedSessionState.Stopping -> true
            else -> false
        }
        fun report(detail: String) {
            android.util.Log.i("OpenLastGame", detail)
            android.widget.Toast.makeText(context, detail, android.widget.Toast.LENGTH_SHORT).show()
        }
        if (busy()) {
            report("A game is already running")
            return@LaunchedEffect
        }
        val game = try {
            withContext(Dispatchers.IO) { graph.gameRepository().getLastLaunchedGame() }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            android.util.Log.e("OpenLastGame", "Cannot read launch history", error)
            report("Could not read the last game")
            return@LaunchedEffect
        }
        if (game == null) {
            report("Launch a game once to create history")
            return@LaunchedEffect
        }
        val installed = withContext(Dispatchers.IO) {
            GameInstallVerifier.canLaunch(context.filesDir, game.relativePath)
        }
        if (!installed) {
            report("The last game is no longer installed")
            return@LaunchedEffect
        }
        // A normal library launch may have won while the database was being read.
        if (busy() || (navController.currentDestination?.route == BachataRoutes.Session &&
                ManagedSession.state.value == ManagedSessionState.Idle)) {
            report("A game launch is already in progress")
            return@LaunchedEffect
        }
        navController.navigate("session/${android.net.Uri.encode(game.id)}") {
            popUpTo(BachataRoutes.Library)
            launchSingleTop = true
        }
        android.util.Log.i("OpenLastGame", "Launch requested: ${game.id}")
    }

    fun goToLibraryClearingSetup() {
        navController.navigate(BachataRoutes.Library) {
            popUpTo(BachataRoutes.Setup) { inclusive = true }
        }
    }

    NavHost(navController = navController, startDestination = startDestination) {
        composable(BachataRoutes.Setup) {
            SetupScreen(onContinue = { goToLibraryClearingSetup() })
        }
        composable(BachataRoutes.Library) {
            LibraryScreen(
                onOpenSettings = { navController.navigate(BachataRoutes.Settings) },
                onOpenGameSettings = { id -> navController.navigate(BachataRoutes.gameSettings(id)) },
                onLaunch = { id -> navController.navigate("session/$id") },
            )
        }
        composable(BachataRoutes.Settings) {
            SettingsScreen(
                onBack = { navController.popBackStack() },
            )
        }
        composable(BachataRoutes.GameSettings) { entry ->
            SettingsScreen(
                initialGameId = requireNotNull(entry.arguments?.getString("id")),
                onBack = { navController.popBackStack() },
            )
        }
        composable(BachataRoutes.Game) {
            LibraryScreen(
                onOpenSettings = { navController.navigate(BachataRoutes.Settings) },
                onOpenGameSettings = { id -> navController.navigate(BachataRoutes.gameSettings(id)) },
                onLaunch = { id -> navController.navigate("session/$id") },
            )
        }
        composable(BachataRoutes.Session) { entry ->
            DisplayModeGate(
                gameId = requireNotNull(entry.arguments?.getString("id")),
                graph = graph, openXrEnabled = openXrEnabled,
                switchActivity = switchDisplayActivity,
                onExit = { if (openXrEnabled) exitXrToLibrary() else navController.popBackStack() },
            )
        }
    }
}

private fun sessionBusy(): Boolean = when (ManagedSession.state.value) {
    is ManagedSessionState.Preparing, is ManagedSessionState.Ready,
    is ManagedSessionState.Running, is ManagedSessionState.Stopping -> true
    else -> false
}

/** Resolve before composing SessionScreen: its ViewModel starts the native service. */
@Composable
private fun DisplayModeGate(gameId: String, graph: BachataNavEntryPoint, openXrEnabled: Boolean,
                            switchActivity: (String, Boolean) -> Unit, onExit: () -> Unit) {
    val context = LocalContext.current
    var admitted by remember(gameId) { mutableStateOf(false) }
    var stereo by remember(gameId) { mutableStateOf(false) }
    var failure by remember(gameId) { mutableStateOf<String?>(null) }
    LaunchedEffect(gameId) {
        try {
            // Activity recreation keeps an existing session's mode, even if settings changed.
            val state = ManagedSession.state.value
            val activeId = when (state) {
                is ManagedSessionState.Running -> state.gameId
                is ManagedSessionState.Ready -> state.gameId
                is ManagedSessionState.Stopping -> state.gameId
                else -> null
            }
            if (sessionBusy()) {
                check(activeId == gameId || state is ManagedSessionState.Preparing) { "A game is already running" }
                // The running session keeps its mode; only the overlays need to know it.
                stereo = try {
                    val running = withContext(Dispatchers.IO) {
                        val game = requireNotNull(graph.gameRepository().getGame(gameId))
                        com.shadps4.android.data.GameDisplayMode.resolve(context.filesDir,
                            graph.runtimeProfiles(), game.id, game.relativePath)
                    }
                    running.psvr && running.effective == com.shadps4.android.runtime.settings.DisplayMode.Mode.TWO_D
                } catch (e: CancellationException) { throw e } catch (e: Exception) { false }
                admitted = true
                return@LaunchedEffect
            }
            val decision = withContext(Dispatchers.IO) {
                val game = requireNotNull(graph.gameRepository().getGame(gameId)) { "Game not found" }
                com.shadps4.android.data.GameDisplayMode.resolve(context.filesDir,
                    graph.runtimeProfiles(), game.id, game.relativePath)
            }
            val xr = decision.effective == com.shadps4.android.runtime.settings.DisplayMode.Mode.XR
            // A PSVR title on the screen shows both eyes side by side.
            stereo = decision.psvr && !xr
            android.util.Log.i("DisplayMode", "$gameId preferred=${decision.preferred} effective=${decision.effective} PSVR=${decision.psvr} forced=${decision.forcedByPsvr}")
            if (xr == openXrEnabled) admitted = true
            else {
                check(!sessionBusy()) { "A game launch is already in progress" }
                switchActivity(gameId, xr)
            }
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { failure = e.message ?: "Could not select display mode" }
    }
    if (admitted) SessionScreen(gameId = gameId, onExit = onExit, stereo = stereo)
    else Column {
        Text(failure ?: "Preparing display mode…")
        TextButton(onClick = onExit) { Text("Back") }
    }
}
