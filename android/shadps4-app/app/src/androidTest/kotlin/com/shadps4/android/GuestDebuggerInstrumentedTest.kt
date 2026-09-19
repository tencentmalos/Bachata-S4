package com.shadps4.android

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.Closeable
import java.io.File
import java.net.Socket
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Ordinary APK, real production loader + FEX + RSP. Enable the opt-in port/wait
 * properties before this selector; no fabricated debugger/backend is used. */
@RunWith(AndroidJUnit4::class)
class GuestDebuggerInstrumentedTest {
    private class Rsp(private val socket: Socket) : Closeable {
        private val input = socket.getInputStream()
        private val output = socket.getOutputStream()
        private fun byte(): Int = input.read().also { check(it >= 0) { "RSP disconnected" } }
        fun send(packet: String) {
            val bytes = packet.toByteArray(Charsets.US_ASCII)
            val checksum = bytes.sumOf { it.toInt() and 255 } and 255
            output.write(("\$$packet#%02x".format(checksum)).toByteArray(Charsets.US_ASCII))
            output.flush()
            check(byte() == '+'.code) { "RSP missing ACK" }
        }
        fun reply(): String {
            check(byte() == '$'.code) { "RSP missing packet" }
            val raw = ArrayList<Int>()
            while (true) {
                val b = byte()
                if (b == '#'.code) break
                check(raw.size < 8192)
                raw.add(b)
            }
            val checksum = "${byte().toChar()}${byte().toChar()}".toInt(16)
            check((raw.sum() and 255) == checksum) { "RSP checksum mismatch" }
            output.write('+'.code); output.flush()
            val decoded = StringBuilder()
            var escaped = false
            for (b in raw) {
                if (escaped) { decoded.append((b xor 32).toChar()); escaped = false }
                else if (b == '}'.code) escaped = true
                else decoded.append(b.toChar())
            }
            check(!escaped)
            return decoded.toString()
        }
        fun command(packet: String): String { send(packet); return reply() }
        fun register(n: Int): ULong {
            val raw = command("p${n.toString(16)}")
            check(raw.length == 16 && raw.all { it.digitToIntOrNull(16) != null }) { raw }
            return raw.chunked(2).reversed().joinToString("").toULong(16)
        }
        override fun close() = socket.close()
    }
    private fun connect(): Rsp {
        val deadline = SystemClock.uptimeMillis() + 10000
        while (true) {
            try { return Rsp(Socket("127.0.0.1", 24680).apply { soTimeout = 3000 }) }
            catch (e: Exception) { if (SystemClock.uptimeMillis() > deadline) throw e; SystemClock.sleep(10) }
        }
    }
    @Test fun realGuestRegistersBreakpointStepAndStopAcrossGenerations() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").path))
        val root = File(context.filesDir, "validation/debugger-${System.nanoTime()}").apply { mkdirs() }
        val entry = File(root, "eboot.bin")
        instrumentation.context.assets.open("wait.elf").use { src -> entry.outputStream().use { src.copyTo(it) } }
        val identity = NativeFexSession.nativeIdentity()
        try {
            if (InstrumentationRegistry.getArguments().getString("debuggerDisabled") == "true") {
                val generation = NativeFexSession.nativeStartExecutable("guest-debugger-off", entry.path)
                assertTrue(generation > 0)
                try {
                    assertEquals(NativeFexSession.WaitPhase.REACHED_TARGET,
                        NativeFexSession.nativeWaitPhase(generation, NativeFexSession.PhaseOrdinal.RUNNING, 10000))
                    try {
                        Socket("127.0.0.1", 24680).use { fail("debugger listener exists while disabled") }
                    } catch (_: java.net.ConnectException) { /* Expected: no listener. */ }
                    SystemClock.sleep(100)
                    NativeFexSession.nativeRequestStop(generation, 3000)
                    assertEquals(NativeFexSession.Outcome.CANCELLED,
                        NativeFexSession.nativeWaitTerminal(generation, 10000))
                    android.util.Log.i("GuestDebuggerAcceptance", "debugger-disabled gen=$generation $identity ${NativeFexSession.nativeTerminalDetail(generation)}")
                } finally { NativeFexSession.nativeRequestStop(generation, 3000) }
                return
            }
            repeat(3) { round ->
                val generation = NativeFexSession.nativeStartExecutable("guest-debugger", entry.path)
                assertTrue(generation > 0)
                try {
                    connect().use { rsp ->
                        assertTrue(rsp.command("qSupported").contains("qXfer:features:read+"))
                        assertTrue(rsp.command("?").startsWith("T05"))
                        assertTrue(rsp.command("qXfer:threads:read::0,1000").contains("guest-stopped"))
                        assertTrue(rsp.command("qXfer:libraries:read::0,1000").contains("eboot.bin"))
                        val start = rsp.register(16)
                        assertTrue(rsp.command("s").startsWith("T05"))
                        assertEquals("one x86 mov instruction", start + 3uL, rsp.register(16))
                        assertEquals("OK", rsp.command("P0=3412000000000000"))
                        assertEquals(0x1234uL, rsp.register(0))
                        val bp = (start + 7uL).toString(16)
                        val original = rsp.command("m$bp,1")
                        assertEquals("OK", rsp.command("Z0,$bp,1"))
                        assertEquals(original, rsp.command("m$bp,1"))
                        assertTrue(rsp.command("c").contains("swbreak:;"))
                        assertEquals(start + 7uL, rsp.register(16))
                        assertEquals("OK", rsp.command("z0,$bp,1"))
                        assertTrue(rsp.command("s").startsWith("T05"))
                        assertEquals("original mov edi instruction", start + 12uL, rsp.register(16))
                        // Real guest memory observation: patch only the stopped
                        // executable, restore original bytes/registers before HLE resumes.
                        val thread = rsp.command("qC").removePrefix("QC")
                        val continuation = rsp.register(16)
                        val savedRax = rsp.register(0)
                        assertTrue(rsp.command("qRegisterInfo1").contains("name:rbx;"))
                        val savedRbx = rsp.register(1)
                        val slot = rsp.register(7) - 8uL
                        val address = start.toString(16)
                        val bytes = rsp.command("m$address,f")
                        fun little(value: ULong) = value.toString(16).padStart(16, '0').chunked(2).reversed().joinToString("")
                        assertEquals("OK", rsp.command("M$address,f:48b82a00000000000000505bccebfe"))
                        assertEquals("OK", rsp.command("P10=${little(start)}"))
                        assertEquals("OK", rsp.command("Z2,${slot.toString(16)},8"))
                        val writeHit = rsp.command("c")
                        assertTrue(writeHit, writeHit.contains("watch:${slot.toString(16)};") && writeHit.contains("watch-size:8;"))
                        assertEquals(start + 11uL, rsp.register(16))
                        assertEquals("OK", rsp.command("z2,${slot.toString(16)},8"))
                        assertEquals("OK", rsp.command("Z3,${slot.toString(16)},8"))
                        val readHit = rsp.command("c")
                        assertTrue(readHit, readHit.contains("rwatch:${slot.toString(16)};"))
                        assertEquals(42uL, rsp.register(1))
                        val mixed = StringBuilder()
                        do {
                            val chunk = rsp.command("qXfer:shadps4-mixed-stack:read:$thread:${mixed.length.toString(16)},1000")
                            assertTrue(chunk.startsWith("m") || chunk.startsWith("l"))
                            mixed.append(chunk.substring(1))
                        } while (chunk.startsWith("m"))
                        val stack = JSONObject(mixed.toString())
                        assertEquals(1, stack.getInt("schemaVersion"))
                        assertEquals(thread.toLong(16), stack.getLong("threadId"))
                        assertEquals("guest", stack.getJSONArray("frames").getJSONObject(0).getString("kind"))
                        assertTrue(stack.getString("limitation").contains("historical"))
                        assertEquals("OK", rsp.command("M$address,4:660fe707"))
                        assertEquals("OK", rsp.command("P10=${little(start)}"))
                        val refusal = rsp.command("c")
                        assertTrue(refusal, refusal.startsWith("T05") && refusal.contains("shadps4-stop:unsupported;"))
                        assertEquals("refused instruction did not retire", start, rsp.register(16))
                        assertEquals("OK", rsp.command("z3,${slot.toString(16)},8"))
                        assertEquals("OK", rsp.command("M$address,f:$bytes"))
                        assertEquals("OK", rsp.command("P0=${little(savedRax)}"))
                        assertEquals("OK", rsp.command("P1=${little(savedRbx)}"))
                        assertEquals("OK", rsp.command("P10=${little(continuation)}"))
                        assertEquals("bad thread", "E22", rsp.command("Hgffffffff"))
                        assertEquals("oversized read", "E22", rsp.command("m$bp,ffffffff"))
                        assertEquals("partial write refused", "E22", rsp.command("P0=12"))
                        assertEquals("unknown packet", "", rsp.command("qNotImplemented"))
                        if (round == 2) assertEquals("OK", rsp.command("D"))
                        // Stop must wake a debugger-parked owner, including the
                        // production Run frame; it must not depend on detaching.
                        NativeFexSession.nativeRequestStop(generation, 3000)
                        val outcome = NativeFexSession.nativeWaitTerminal(generation, 10000)
                        val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                        assertEquals(detail, NativeFexSession.Outcome.CANCELLED, outcome)
                        android.util.Log.i("GuestDebuggerAcceptance", "round=$round gen=$generation start=0x${start.toString(16)} outcome=$outcome $identity $detail")
                    }
                } finally { NativeFexSession.nativeRequestStop(generation, 3000) }
                assertEquals(identity, NativeFexSession.nativeIdentity())
            }
        } finally { if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively() }
    }
}
