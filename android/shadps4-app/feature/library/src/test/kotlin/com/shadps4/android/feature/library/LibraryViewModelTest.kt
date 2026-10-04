package com.shadps4.android.feature.library

import com.shadps4.android.model.Game
import com.shadps4.android.runtime.input.NavControllerEvent
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LibraryViewModelTest {
    @Test fun detailOptionNavigationDoesNotLaunchWhileChangingAChoice() = runTest {
        val model = LibraryViewModel()
        model.setGames(listOf(game("A", "Alpha")))
        model.showDetails("A")
        model.setLaunchOptionCount(5)
        val changes = mutableListOf<Pair<Int, Int>>()
        val launches = mutableListOf<String>()
        backgroundScope.launch { model.adjustLaunchOption.collect { changes.add(it) } }
        backgroundScope.launch { model.launch.collect { launches.add(it) } }
        runCurrent()
        model.handleNavEvent(NavControllerEvent("dpad_down", pressed = true))
        model.handleNavEvent(NavControllerEvent("cross", pressed = true))
        model.handleNavEvent(NavControllerEvent("dpad_left", pressed = true))
        runCurrent()
        // Cross activates the row (next choice, or the Guest Patches panel); left steps back.
        assertEquals(listOf(0 to 0, 0 to -1), changes)
        assertTrue(launches.isEmpty())
        model.handleNavEvent(NavControllerEvent("dpad_up", pressed = true))
        model.handleNavEvent(NavControllerEvent("cross", pressed = true))
        runCurrent()
        assertEquals(listOf("A"), launches)
    }

    @Test fun guestPatchPanelKeepsTheControllerUntilCircle() = runTest {
        val model = LibraryViewModel()
        model.setGames(listOf(game("A", "Alpha")))
        model.showDetails("A")
        model.setLaunchOptionCount(4) // three settings, then Guest Patches
        val changes = mutableListOf<Pair<Int, Int>>()
        val launches = mutableListOf<String>()
        backgroundScope.launch { model.adjustLaunchOption.collect { changes.add(it) } }
        backgroundScope.launch { model.launch.collect { launches.add(it) } }
        runCurrent()
        repeat(4) { model.handleNavEvent(NavControllerEvent("dpad_down", pressed = true)) }
        assertEquals(3, model.state.value.launchOptionIndex)
        model.openLaunchSubPanel()
        model.setLaunchOptionCount(2) // two packages
        assertTrue(model.state.value.launchSubPanel)
        assertEquals(0, model.state.value.launchOptionIndex)
        repeat(3) { model.handleNavEvent(NavControllerEvent("dpad_down", pressed = true)) }
        assertEquals(1, model.state.value.launchOptionIndex)
        repeat(3) { model.handleNavEvent(NavControllerEvent("dpad_up", pressed = true)) }
        assertEquals(0, model.state.value.launchOptionIndex) // never the Launch button
        model.handleNavEvent(NavControllerEvent("cross", pressed = true))
        model.handleNavEvent(NavControllerEvent("square", pressed = true))
        runCurrent()
        assertEquals(listOf(0 to 0), changes)
        model.handleNavEvent(NavControllerEvent("circle", pressed = true))
        assertFalse(model.state.value.launchSubPanel)
        assertEquals(3, model.state.value.launchOptionIndex)
        assertEquals("A", model.state.value.showDetailsGameId)
        assertTrue(launches.isEmpty())
        model.handleNavEvent(NavControllerEvent("circle", pressed = true))
        assertEquals(null, model.state.value.showDetailsGameId)
    }

    @Test
    fun sortsGamesByTitleThenId() {
        val viewModel = LibraryViewModel()

        viewModel.setGames(
            listOf(
                Game(id = "CUSA3", title = "zeta", relativePath = "games/CUSA3"),
                Game(id = "CUSA2", title = "Alpha", relativePath = "games/CUSA2"),
                Game(id = "CUSA1", title = "alpha", relativePath = "games/CUSA1"),
            ),
        )

        assertEquals(listOf("CUSA1", "CUSA2", "CUSA3"), viewModel.state.value.games.map { it.id })
    }

    @Test
    fun keepsSelectionWhenPresentOtherwiseSelectsFirstSortedGame() {
        val viewModel = LibraryViewModel()

        viewModel.setGames(listOf(game("B", "Beta"), game("A", "Alpha")))
        assertEquals("A", viewModel.state.value.selectedGameId)

        viewModel.selectGame("B")
        viewModel.setGames(listOf(game("B", "Beta"), game("A", "Alpha")))
        assertEquals("B", viewModel.state.value.selectedGameId)
    }

    @Test
    fun sharePressedEmitsToggleOrientationWhenNoDetails() = runTest {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha")))
        val events = mutableListOf<Unit>()
        val job = backgroundScope.launch {
            viewModel.toggleOrientation.collect { events.add(it) }
        }
        yield()
        runCurrent()

        val handled = viewModel.handleNavEvent(NavControllerEvent("share", pressed = true))
        runCurrent()

        assertTrue(handled)
        assertEquals(1, events.size)
        job.cancel()
    }

    @Test
    fun sharePressedDoesNotEmitWhenDetailsOpen() = runTest {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha")))
        viewModel.showDetails("A")
        val events = mutableListOf<Unit>()
        val job = backgroundScope.launch {
            viewModel.toggleOrientation.collect { events.add(it) }
        }
        yield()
        runCurrent()

        val handled = viewModel.handleNavEvent(NavControllerEvent("share", pressed = true))
        runCurrent()

        assertTrue(handled)
        assertEquals(0, events.size)
        job.cancel()
    }

    @Test
    fun shareReleaseIsIgnored() = runTest {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha")))
        val events = mutableListOf<Unit>()
        val job = backgroundScope.launch {
            viewModel.toggleOrientation.collect { events.add(it) }
        }
        yield()
        runCurrent()

        val handled = viewModel.handleNavEvent(NavControllerEvent("share", pressed = false))
        runCurrent()

        assertFalse(handled)
        assertEquals(0, events.size)
        job.cancel()
    }

    @Test
    fun dpadRightStillNavigatesAfterShareWiring() {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha"), game("B", "Beta")))
        assertEquals("A", viewModel.state.value.selectedGameId)

        val handled = viewModel.handleNavEvent(NavControllerEvent("dpad_right", pressed = true))

        assertTrue(handled)
        assertEquals("B", viewModel.state.value.selectedGameId)
    }

    private fun game(id: String, title: String) = Game(id, title, "games/$id")
}
