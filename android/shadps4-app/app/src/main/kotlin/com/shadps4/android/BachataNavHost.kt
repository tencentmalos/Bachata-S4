package com.shadps4.android

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
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
}

@Composable
fun BachataNavHost(startDestination: String = BachataRoutes.Setup, openLastGameRequest: Int = 0) {
    val navController = rememberNavController()
    val context = LocalContext.current
    val graph = remember {
        EntryPointAccessors.fromApplication(
            context.applicationContext,
            BachataNavEntryPoint::class.java,
        )
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
            SessionScreen(
                gameId = requireNotNull(entry.arguments?.getString("id")),
                onExit = { navController.popBackStack() },
            )
        }
    }
}
