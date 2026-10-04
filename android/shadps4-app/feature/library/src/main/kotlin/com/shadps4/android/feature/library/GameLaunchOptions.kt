package com.shadps4.android.feature.library

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.shadps4.android.designsystem.theme.BachataPalette
import com.shadps4.android.runtime.settings.*
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive

/** Draft is committed once, before launching; cancelling the sheet does not change preferences. */
data class GameLaunchOptions(
    val gameId: String? = null,
    val loaded: Boolean = false,
    val saving: Boolean = false,
    val psvr: Boolean = false,
    val global: RuntimeProfile = RuntimeProfile(),
    val game: RuntimeProfile = RuntimeProfile(),
    val changes: Map<String, JsonElement> = emptyMap(),
    val outputExtents: List<Pair<Int, Int>> = emptyList(),
    /** Package files of this game (host/guest_patches/<TITLE_ID>/), by file stem. */
    val patches: List<GuestPatchPackage> = emptyList(),
    val error: String? = null,
) {
    val ready get() = loaded && !saving && error == null
    private val effectiveGame get() = game.copy(values = game.values + changes)
    val xr get() = DisplayMode.resolve(global, effectiveGame, psvr).effective == DisplayMode.Mode.XR
    fun specs(): List<RuntimeSettingSpec> {
        val ids = buildList {
            if (!psvr) add(DisplayMode.ID)
            add(if (xr) InternalScale.XR_ID else InternalScale.ID)
            if (xr) addAll(listOf(XrRendering.UPSCALER, XrRendering.OUTPUT, XrRendering.FOVEATION, XrRendering.STATUS))
            else add(XrRendering.SCREEN_UPSCALER)
        }
        val catalog = RuntimeSettingCatalog.loadAndroidSettings().associateBy { it.id }
        return ids.map { catalog.getValue(it) }
    }
    fun value(spec: RuntimeSettingSpec): String {
        val raw = changes[spec.id] ?: game.values[spec.id] ?: global.values[spec.id] ?: spec.defaultValue
        return if (spec.id == XrRendering.STATUS) XrRendering.statusChoice(raw) else (raw as JsonPrimitive).content
    }
    fun select(spec: RuntimeSettingSpec, choice: String): GameLaunchOptions {
        require(spec.id in specs().map { it.id } && choice in spec.choices)
        val value = JsonPrimitive(choice)
        return copy(changes = changes + (spec.id to value), error = null)
    }
    fun applyTo(current: RuntimeProfile) = current.copy(values = current.values + changes)
    fun choiceLabel(spec: RuntimeSettingSpec, choice: String): String {
        val label = spec.choiceLabel(choice)
        if (spec.id != XrRendering.OUTPUT) return label
        val size = outputExtents.getOrNull(spec.choices.indexOf(choice)) ?: return label
        return "$label\n${size.first} × ${size.second}"
    }

    /** Selected packages in install order, which is the order of [patchChoices]. */
    fun patchSelection(): List<String> = GuestPatches.selection(
        changes[GuestPatches.ID] ?: game.values[GuestPatches.ID] ?: global.values[GuestPatches.ID])
    /** The game's packages, then selected names without a package file (so they can be removed). */
    fun patchChoices(): List<GuestPatchPackage> =
        patches + patchSelection().filter { name -> patches.none { it.name == name } }
            .map { GuestPatchPackage(it, it, missing = true) }
    /** Why [choice] cannot be switched on with the current selection, or null. */
    fun patchBlocker(choice: GuestPatchPackage): String? {
        if (choice.missing) return "File not found"
        val selected = patchSelection()
        val other = choice.conflicts.firstOrNull { it in selected } ?: return null
        return "Changes the same code as ${patches.firstOrNull { it.name == other }?.label ?: other}"
    }
    /** Switches a package; a selected package can always be switched off. */
    fun togglePatch(name: String): GameLaunchOptions {
        val choices = patchChoices()
        val choice = choices.firstOrNull { it.name == name } ?: return this
        val selected = patchSelection()
        val on = name !in selected
        if (on && patchBlocker(choice) != null) return this
        val next = if (on) selected + name else selected - name
        val ordered = choices.map { it.name }.filter { it in next }
        return copy(changes = changes + (GuestPatches.ID to GuestPatches.encode(ordered)), error = null)
    }
    /** Controller rows: the settings, then Guest Patches when the game has packages; in the
     *  Guest Patches panel, its packages. */
    fun rowCount(patchPanel: Boolean): Int =
        if (patchPanel) patchChoices().size else specs().size + if (patchChoices().isEmpty()) 0 else 1
}

/** Actions of the Guest Patches row and its panel. */
sealed interface PatchAction {
    data object Open : PatchAction
    data object Close : PatchAction
    data class Toggle(val name: String) : PatchAction
}

/** One focusable row of the launch panel; the controller focus scrolls it into view. */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
private fun LaunchOptionRow(key: Any, focused: Boolean, onClick: (() -> Unit)? = null,
                            content: @Composable ColumnScope.() -> Unit) {
    val bringIntoView = remember(key) { BringIntoViewRequester() }
    LaunchedEffect(focused, key) {
        if (focused) bringIntoView.bringIntoView()
    }
    Surface(modifier = Modifier.bringIntoViewRequester(bringIntoView)
                .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier),
            color = BachataPalette.RaisedSurface,
            border = BorderStroke(1.dp, if (focused) BachataPalette.Accent else Color.Transparent),
            shape = MaterialTheme.shapes.small) {
        Column(Modifier.fillMaxWidth().padding(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp),
               content = content)
    }
}

@Composable
internal fun GameLaunchOptionsPanel(options: GameLaunchOptions, focused: Int,
                                    onSelect: (RuntimeSettingSpec, String) -> Unit,
                                    patchPanel: Boolean = false,
                                    onPatch: (PatchAction) -> Unit = {}) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(if (patchPanel) "Guest patches" else "Launch options",
            style = MaterialTheme.typography.titleSmall, color = BachataPalette.Primary)
        when {
            options.error != null -> Text(options.error, color = MaterialTheme.colorScheme.error)
            !options.loaded -> Text("Reading game display mode…", color = BachataPalette.Secondary)
            patchPanel -> GuestPatchPanel(options, focused, onPatch)
            else -> {
                if (options.psvr) Text("PSVR · Immersive XR", color = BachataPalette.Secondary)
                val specs = options.specs()
                specs.forEachIndexed { index, spec ->
                    LaunchOptionRow(options.gameId to spec.id, focused == index) {
                        Text(spec.title,
                            style = MaterialTheme.typography.bodyMedium, color = BachataPalette.Primary)
                        if (spec.id == XrRendering.OUTPUT && options.outputExtents.isNotEmpty())
                            Text("Per eye · last verified", style = MaterialTheme.typography.labelSmall, color = BachataPalette.Secondary)
                        Row(Modifier.fillMaxWidth().selectableGroup(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            spec.choices.forEach { choice ->
                                val selected = options.value(spec) == choice
                                Surface(
                                    modifier = Modifier.weight(1f).selectable(
                                        selected = selected, enabled = options.ready,
                                        role = Role.RadioButton, onClick = { onSelect(spec, choice) }),
                                    color = if (selected) BachataPalette.Accent else BachataPalette.Surface,
                                    shape = MaterialTheme.shapes.small,
                                ) {
                                    Text(options.choiceLabel(spec, choice),
                                        modifier = Modifier.heightIn(min = 40.dp).padding(horizontal = 6.dp, vertical = 10.dp),
                                        style = MaterialTheme.typography.labelMedium,
                                        textAlign = TextAlign.Center,
                                        color = if (selected) BachataPalette.OnAccent else BachataPalette.Primary)
                                }
                            }
                        }
                    }
                }
                val choices = options.patchChoices()
                if (choices.isNotEmpty()) {
                    LaunchOptionRow(options.gameId to "guest_patches", focused == specs.size,
                        onClick = { if (options.ready) onPatch(PatchAction.Open) }) {
                        Text("Guest Patches",
                            style = MaterialTheme.typography.bodyMedium, color = BachataPalette.Primary)
                        val selected = options.patchSelection()
                        val on = choices.filter { it.name in selected }.map { it.label }
                        Text(if (on.isEmpty()) "Off · ${choices.size} available"
                             else "${on.size} of ${choices.size} on: ${on.joinToString(", ")}",
                            style = MaterialTheme.typography.labelMedium, color = BachataPalette.Secondary)
                    }
                }
                Text(if (options.saving) "Saving…" else "Saved for this game when you launch.",
                    style = MaterialTheme.typography.bodySmall, color = BachataPalette.Secondary)
            }
        }
    }
}

/** Second level of the launch panel: every package of the game with its switch. */
@Composable
private fun GuestPatchPanel(options: GameLaunchOptions, focused: Int, onPatch: (PatchAction) -> Unit) {
    val selected = options.patchSelection()
    options.patchChoices().forEachIndexed { index, choice ->
        val on = choice.name in selected
        val blocker = options.patchBlocker(choice)
        val enabled = options.ready && (on || blocker == null)
        LaunchOptionRow(options.gameId to "patch:${choice.name}", focused == index,
            onClick = if (enabled) ({ onPatch(PatchAction.Toggle(choice.name)) }) else null) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(choice.label, style = MaterialTheme.typography.bodyMedium,
                        color = if (enabled) BachataPalette.Primary else BachataPalette.Secondary)
                    Text(choice.name, style = MaterialTheme.typography.labelSmall, color = BachataPalette.Secondary)
                }
                Surface(color = if (on) BachataPalette.Accent else BachataPalette.Surface,
                        shape = MaterialTheme.shapes.small) {
                    Text(if (on) "On" else "Off",
                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 6.dp),
                        style = MaterialTheme.typography.labelMedium,
                        color = if (on) BachataPalette.OnAccent else BachataPalette.Primary)
                }
            }
            if (choice.description.isNotEmpty())
                Text(choice.description, style = MaterialTheme.typography.bodySmall, color = BachataPalette.Secondary)
            if (blocker != null)
                Text(blocker, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        }
    }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text("The game version is checked at start; a package for another version is skipped.",
            modifier = Modifier.weight(1f),
            style = MaterialTheme.typography.bodySmall, color = BachataPalette.Secondary)
        TextButton(onClick = { onPatch(PatchAction.Close) }) { Text("Done") }
    }
}
