package com.shadps4.android.data

import android.content.Context
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import com.shadps4.android.runtime.session.NativeFexSession
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Reports one play session of an EmuPSP-managed game (EmuPSP play-time protocol v1, docs/playtime-protocol.md):
 * `start` when the ROM lease opens, a `heartbeat` every minute, `stop` when the lease closes. Played time is the
 * session's monotonic wall time; pauses are not subtracted in v1. `stop` carries the session's outcome, and every
 * report carries presented / slow frame counts once a frame source is attached. Best effort: every call runs on its own
 * thread and a missing or older EmuPSP only produces a log line, never an effect on the game.
 */
class EmuPspPlaytime private constructor(private val context: Context, private val tree: Uri, private val gameId: String) : AutoCloseable {
    private val session = UUID.randomUUID().toString()
    private val started = SystemClock.elapsedRealtime()
    private val closed = AtomicBoolean(false)
    private val worker = Executors.newSingleThreadScheduledExecutor { Thread(it, "EmuPSP playtime").apply { isDaemon = true } }
    private val version = runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName }.getOrNull().orEmpty()
    /** exit / crash / error, sent with `stop`; see [outcomeOf]. */
    @Volatile var outcome: String? = null
    @Volatile private var frameSource: (() -> LongArray?)? = null
    @Volatile private var frameBaseline: LongArray? = null

    /**
     * Attaches the process-wide [presented, slow] frame counters. The reading at attach time is the session's baseline,
     * so each report sends the frames of this session only.
     */
    fun frames(source: () -> LongArray?) {
        frameBaseline = runCatching { source() }.getOrNull()?.takeIf { it.size >= 2 }
        frameSource = source
    }

    private fun report(event: String) {
        val extras = Bundle().apply {
            putInt("protocol_version", VERSION); putString("event", event); putString("session_id", session); putString("game_id", gameId)
            putLong("played_ms", SystemClock.elapsedRealtime() - started); putString("emulator", "shadPS4"); putString("emulator_version", version.take(64))
            val baseline = frameBaseline
            val now = frameSource?.let { runCatching { it() }.getOrNull() }?.takeIf { it.size >= 2 }
            if (baseline != null && now != null && now[0] >= baseline[0] && now[1] >= baseline[1]) {
                putLong("frames", now[0] - baseline[0]); putLong("slow_frames", now[1] - baseline[1]); putInt("slow_frame_ms", SLOW_FRAME_MS)
            }
            if (event == "stop") outcome?.let { putString("outcome", it) }
        }
        val reply = runCatching { context.contentResolver.call(tree, METHOD, tree.toString(), extras) }
        val ok = reply.getOrNull()?.getBoolean("ok") == true
        if (!ok) android.util.Log.w("ManagedRom", "playtime $event for $gameId not recorded: " +
            (reply.exceptionOrNull()?.javaClass?.simpleName ?: reply.getOrNull()?.getString("error") ?: "no reply"))
    }

    /** Sends `stop` with the final played time (and outcome, when known) and ends the session. Safe to call more than once. */
    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        runCatching { worker.execute { report("stop") }; worker.shutdown() }
    }

    companion object {
        const val METHOD = "emupsp.play_session"
        const val VERSION = 1
        /** The native counter's slow-frame threshold: present intervals above this count as slow. */
        const val SLOW_FRAME_MS = 50
        private const val HEARTBEAT_SECONDS = 60L
        /** Maps a session terminal ([NativeFexSession.Outcome]) to the protocol outcome. */
        fun outcomeOf(code: Int): String = when (code) {
            NativeFexSession.Outcome.RETURNED, NativeFexSession.Outcome.CANCELLED -> "exit"
            NativeFexSession.Outcome.FAULTED, NativeFexSession.Outcome.UNEXPECTED -> "crash"
            else -> "error"
        }
        /** Starts reporting for [gameId] (`ps4:CUSA12345`) through the producer of the granted [tree]. */
        fun start(context: Context, tree: Uri, gameId: String): EmuPspPlaytime =
            EmuPspPlaytime(context.applicationContext, tree, gameId).apply {
                runCatching {
                    worker.execute { report("start") }
                    worker.scheduleAtFixedRate({ if (!closed.get()) report("heartbeat") }, HEARTBEAT_SECONDS, HEARTBEAT_SECONDS, TimeUnit.SECONDS)
                }
            }
    }
}
