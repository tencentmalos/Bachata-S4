package com.shadps4.android.feature.library

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import androidx.compose.runtime.*
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
}

@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
@Composable
internal fun GameLaunchOptionsPanel(options: GameLaunchOptions, focused: Int,
                                    onSelect: (RuntimeSettingSpec, String) -> Unit) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text("Launch options", style = MaterialTheme.typography.titleSmall, color = BachataPalette.Primary)
        when {
            options.error != null -> Text(options.error, color = MaterialTheme.colorScheme.error)
            !options.loaded -> Text("Reading game display mode…", color = BachataPalette.Secondary)
            else -> {
                if (options.psvr) Text("PSVR · Immersive XR", color = BachataPalette.Secondary)
                options.specs().forEachIndexed { index, spec ->
                    val bringIntoView = remember(options.gameId, spec.id) { BringIntoViewRequester() }
                    LaunchedEffect(focused, spec.id) {
                        if (focused == index) bringIntoView.bringIntoView()
                    }
                    Surface(modifier = Modifier.bringIntoViewRequester(bringIntoView),
                            color = BachataPalette.RaisedSurface,
                            border = BorderStroke(1.dp, if (focused == index) BachataPalette.Accent else Color.Transparent),
                            shape = MaterialTheme.shapes.small) {
                        Column(Modifier.fillMaxWidth().padding(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
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
                }
                Text(if (options.saving) "Saving…" else "Saved for this game when you launch.",
                    style = MaterialTheme.typography.bodySmall, color = BachataPalette.Secondary)
            }
        }
    }
}
