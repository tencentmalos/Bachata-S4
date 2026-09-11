package com.shadps4.fexvalidation

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.concurrent.thread

/**
 * Ordinary validation activity. It loads the same guest_cpu_fex backend the CLI uses (inside this
 * normal app process), reports in-process identity, and drives a real x86-64 guest increment on a
 * background thread with Start/Stop semantics (the UI thread never blocks on the guest).
 *
 * Every line shown in the UI is also mirrored to logcat under [TAG] so an on-device validation run
 * can be captured with `adb logcat -s FexValidation` without depending on the UI (useful on
 * multi-display devices where uiautomator dumps are unreliable). On first launch the basic fixture
 * is run [AUTO_RUNS] times automatically to produce the A0-b in-process records; the Run button
 * repeats a single iteration on demand.
 */
class MainActivity : Activity() {
    private lateinit var output: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 48, 32, 32)
        }
        output = TextView(this).apply {
            textSize = 14f
        }
        val runButton = Button(this).apply {
            text = "Run guest increment"
            setOnClickListener { runGuest(iterations = 1, label = "manual") }
        }
        val scroll = ScrollView(this).apply { addView(output) }
        root.addView(runButton)
        root.addView(scroll)
        setContentView(root)

        emit("shadPS4 FEX validation app")
        emit("device API=${Build.VERSION.SDK_INT} model=${Build.MODEL} targetSdk=${applicationInfo.targetSdkVersion}")

        // Identity is cheap and safe to read on creation (no guest run).
        try {
            emit("native: " + NativeBridge.nativeIdentity())
            emit("native page_size(syscall)=${NativeBridge.nativePageSize()}")
            emit("va-gaps:\n" + NativeBridge.nativeVaGaps())
        } catch (t: Throwable) {
            emit("native load failed: ${t.message}")
            return
        }

        // Auto-run the basic fixture to produce device evidence without needing a UI tap.
        runGuest(iterations = AUTO_RUNS, label = "auto")
    }

    private fun runGuest(iterations: Int, label: String) {
        thread(name = "fex-validation-runner-$label") {
            // Optional startup delay so a native debugger can attach before the guest run. Controlled
            // by the system property `debug.fexval.startdelay` (seconds); defaults to 0 = no delay,
            // so normal runs are unaffected. Set with: adb shell setprop debug.fexval.startdelay 15
            if (label == "auto") {
                val delaySec = runCatching {
                    val p = Runtime.getRuntime().exec(arrayOf("getprop", "debug.fexval.startdelay"))
                    p.inputStream.bufferedReader().readText().trim().toIntOrNull() ?: 0
                }.getOrDefault(0)
                if (delaySec > 0) {
                    emit("[auto] holding ${delaySec}s for debugger attach (pid=${android.os.Process.myPid()})")
                    Thread.sleep(delaySec * 1000L)
                }
            }
            var passed = 0
            for (i in 1..iterations) {
                val input = 40L + i
                val expected = input + 1
                val err = arrayOfNulls<String>(1)
                val started = System.nanoTime()
                val result = NativeBridge.runGuestIncrement(input, err)
                val elapsedMs = (System.nanoTime() - started) / 1_000_000
                val line = if (result == expected) {
                    passed++
                    "[$label $i/$iterations] PASS input=$input output=$result (expected $expected) ${elapsedMs}ms"
                } else if (result >= 0) {
                    "[$label $i/$iterations] FAIL input=$input output=$result (expected $expected) ${elapsedMs}ms"
                } else {
                    "[$label $i/$iterations] ERROR input=$input: ${err[0] ?: "unknown"} ${elapsedMs}ms"
                }
                emit(line)
            }
            emit("[$label] summary: $passed/$iterations passed")
        }
    }

    private fun emit(line: String) {
        Log.i(TAG, line)
        runOnUiThread { output.append(line + "\n") }
    }

    private companion object {
        const val TAG = "FexValidation"
        const val AUTO_RUNS = 10
    }
}
