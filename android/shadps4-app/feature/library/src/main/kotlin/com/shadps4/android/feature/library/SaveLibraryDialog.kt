package com.shadps4.android.feature.library

import android.text.format.DateFormat
import android.text.format.Formatter
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.shadps4.android.data.GameSaveLibrary
import com.shadps4.android.data.RuntimeProfileStore
import com.shadps4.android.data.SaveEntry
import com.shadps4.android.data.SaveLibrary
import com.shadps4.android.model.Game
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Date

@Composable
internal fun SaveLibraryDialog(game: Game, profiles: RuntimeProfileStore, onDismiss: () -> Unit,
    onBackHandler: (() -> Unit) -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var source by remember(game.id) { mutableStateOf<GameSaveLibrary?>(null) }
    var entries by remember(game.id) { mutableStateOf<List<SaveEntry>>(emptyList()) }
    var selected by remember(game.id) { mutableStateOf<SaveEntry?>(null) }
    var note by remember(game.id) { mutableStateOf("") }
    var busy by remember { mutableStateOf(true) }
    var error by remember { mutableStateOf<String?>(null) }
    var revision by remember { mutableIntStateOf(0) }
    var discard by remember { mutableStateOf(false) }
    val dirty = selected?.let { note != it.note } ?: false
    val close = { if (!busy) { if (dirty) discard = true else onDismiss() } }
    SideEffect { onBackHandler(close) }
    LaunchedEffect(game.id, revision) {
        busy = true
        error = null
        try {
            val result = withContext(Dispatchers.IO) {
                val opened = GameSaveLibrary.open(context.filesDir, profiles, game)
                opened to opened.library.list(opened.titleId)
            }
            source = result.first
            entries = result.second
        } catch (e: kotlinx.coroutines.CancellationException) { throw e }
        catch (e: Exception) { error = e.message ?: "无法读取存档" }
        finally { busy = false }
    }
    Dialog(onDismissRequest = close, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
      val keyboardOpen = WindowInsets.ime.getBottom(LocalDensity.current) > 0
      Box(Modifier.fillMaxSize().systemBarsPadding().imePadding()
          .padding(bottom = if (keyboardOpen) 24.dp else 0.dp), contentAlignment = Alignment.Center) {
        Surface(modifier = Modifier.fillMaxWidth(0.94f).fillMaxHeight(0.92f),
            shape = MaterialTheme.shapes.extraLarge,
            color = com.shadps4.android.designsystem.theme.BachataPalette.Surface.copy(alpha = 1f)) {
            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("存档管理 · ${game.title}", style = MaterialTheme.typography.titleLarge,
                        modifier = Modifier.weight(1f), maxLines = 2)
                    TextButton(onClick = close, enabled = !busy) { Text("关闭") }
                }
                error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                val save = selected
                if (save == null) {
                    source?.let { Text("${it.titleId} · ${it.home}", style = MaterialTheme.typography.bodySmall) }
                    Row {
                        TextButton(onClick = { revision++ }, enabled = !busy) { Text("刷新") }
                        Text("${entries.size} 个存档", modifier = Modifier.padding(12.dp))
                    }
                    if (!busy && error == null && entries.isEmpty()) Text("该游戏还没有本地存档")
                    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        items(entries, key = { it.path }) { entry ->
                            OutlinedCard(Modifier.fillMaxWidth().clickable(enabled = !busy) {
                                selected = entry; note = entry.note; error = null
                            }.testTag("save-${entry.userId}-${entry.directory}")) {
                                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                    Text(entry.subtitle ?: entry.title ?: entry.directory,
                                        style = MaterialTheme.typography.titleMedium)
                                    Text("用户 ${entry.userId} · ${entry.directory} · ${Formatter.formatFileSize(context, entry.bytes)}")
                                    Text(DateFormat.format("yyyy-MM-dd HH:mm:ss", Date(entry.modifiedMs)).toString())
                                    if (entry.note.isNotBlank()) Text(entry.note, maxLines = 2,
                                        color = MaterialTheme.colorScheme.primary)
                                    if (entry.warnings.isNotEmpty()) Text("部分文件无法读取，请查看详情",
                                        color = MaterialTheme.colorScheme.error)
                                }
                            }
                        }
                    }
                } else {
                    LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        item {
                            SelectionContainer {
                                Text(listOfNotNull(save.title, save.subtitle, save.detail).filter { it.isNotBlank() }.joinToString("\n") + "\n" +
                                    "用户 ${save.userId} · ${save.files.size} 个文件 · ${Formatter.formatFileSize(context, save.bytes)}\n${save.path}")
                            }
                        }
                        item {
                            OutlinedTextField(value = note, onValueChange = {
                                if (it.length <= SaveLibrary.MAX_NOTE_CHARS) note = it
                            }, enabled = !busy, label = { Text("备注") },
                                supportingText = { Text("独立保存，不修改游戏存档 · ${note.length}/${SaveLibrary.MAX_NOTE_CHARS}") },
                                modifier = Modifier.fillMaxWidth().testTag("save-note"), minLines = 2, maxLines = 5)
                        }
                        items(save.warnings) { Text(it, color = MaterialTheme.colorScheme.error) }
                        items(save.files, key = { it.path }) { file ->
                            SelectionContainer {
                                Text("${file.path} · ${Formatter.formatFileSize(context, file.bytes)} · " +
                                    DateFormat.format("yyyy-MM-dd HH:mm:ss", Date(file.modifiedMs)),
                                    style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        TextButton(enabled = !busy, onClick = { selected = null; note = ""; error = null }) {
                            Text(if (dirty) "取消编辑" else "返回列表")
                        }
                        Button(enabled = dirty && !busy, modifier = Modifier.testTag("save-note-submit"), onClick = {
                            busy = true; error = null
                            scope.launch {
                                try {
                                    withContext(Dispatchers.IO) { requireNotNull(source).library.writeNote(save, note) }
                                    val updated = save.copy(note = note)
                                    selected = updated
                                    entries = entries.map { if (it.path == save.path) updated else it }
                                } catch (e: kotlinx.coroutines.CancellationException) { throw e }
                                catch (e: Exception) { error = e.message ?: "备注保存失败" }
                                finally { busy = false }
                            }
                        }) { Text("保存备注") }
                    }
                }
            }
        }
    }
    }
    if (discard) AlertDialog(onDismissRequest = { discard = false }, title = { Text("放弃未保存的备注？") },
        confirmButton = { TextButton(onClick = onDismiss) { Text("放弃") } },
        dismissButton = { TextButton(onClick = { discard = false }) { Text("继续编辑") } })
}
