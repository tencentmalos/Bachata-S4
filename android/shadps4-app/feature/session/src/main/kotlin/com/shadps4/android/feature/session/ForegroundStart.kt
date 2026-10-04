package com.shadps4.android.feature.session

import androidx.lifecycle.Lifecycle
import androidx.lifecycle.withResumed
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Calls [tryStart] while [lifecycle] is resumed until it returns true, and returns how many
 * attempts it refused. A refused attempt waits for the lifecycle to leave and re-enter the resumed
 * state, or for [retryMs] if it stays resumed. Cancelled if the lifecycle is destroyed first.
 */
internal suspend fun startWhenResumed(lifecycle: Lifecycle, retryMs: Long = 1_000, tryStart: () -> Boolean): Int {
    var refusals = 0
    while (!lifecycle.withResumed { tryStart() }) {
        refusals++
        withTimeoutOrNull(retryMs) { lifecycle.currentStateFlow.first { it != Lifecycle.State.RESUMED } }
    }
    return refusals
}
