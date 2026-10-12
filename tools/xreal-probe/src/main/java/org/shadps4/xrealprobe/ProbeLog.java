package org.shadps4.xrealprobe;

import android.content.Context;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import org.json.JSONObject;
import java.io.*;
import java.text.SimpleDateFormat;
import java.util.*;

/** On-device evidence survives unplugging USB and native process crashes. */
public final class ProbeLog {
    public static File directory;
    private static BufferedWriter events, poses;
    private static long sequence;
    private static java.lang.Process logcat;

    static void init(Context context) throws Exception {
        String id = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()) + "-" + Process.myPid();
        directory = new File(context.getFilesDir(), "runs/" + id);
        if (!directory.mkdirs()) throw new IOException("Cannot create " + directory);
        events = writer("events.jsonl"); poses = writer("poses.jsonl");
        Thread.setDefaultUncaughtExceptionHandler((thread,error) -> {
            event("java_fatal", "thread", thread.getName(), "error", Log.getStackTraceString(error));
            Process.killProcess(Process.myPid());
        });
        // Apps may read their own UID's logcat. Capture SDK diagnostics even with no adb attached.
        logcat = new ProcessBuilder("logcat", "--pid=" + Process.myPid(), "-v", "threadtime", "-T", "1")
                .redirectErrorStream(true).start();
        Thread capture = new Thread(() -> {
            try (BufferedReader input = new BufferedReader(new InputStreamReader(logcat.getInputStream()))) {
                BufferedWriter output = writer("sdk-logcat.txt");
                long bytes = 0; String line;
                while ((line = input.readLine()) != null) {
                    output.write(line); output.newLine(); output.flush(); bytes += line.length() + 1;
                    if (bytes > 8 * 1024 * 1024) {
                        output.close(); rotate("sdk-logcat.txt"); output = writer("sdk-logcat.txt"); bytes=0;
                    }
                }
                output.close();
            } catch (Exception e) { event("logcat_capture_error", "error", e.toString()); }
        }, "probe-logcat");
        capture.setDaemon(true); capture.start();
        event("run_start", "run_id", id, "pid", Process.myPid(), "schema", 1);
    }
    private static BufferedWriter writer(String name) throws IOException {
        return new BufferedWriter(new OutputStreamWriter(new FileOutputStream(new File(directory,name),true), "UTF-8"));
    }
    private static void rotate(String name) {
        new File(directory,name+".3").delete();
        for(int i=2;i>=1;i--) new File(directory,name+"."+i).renameTo(new File(directory,name+"."+(i+1)));
        new File(directory,name).renameTo(new File(directory,name+".1"));
    }
    public static JSONObject object(Object... pairs) {
        JSONObject out=new JSONObject();
        try { for(int i=0;i<pairs.length;i+=2) out.put((String)pairs[i],pairs[i+1]); }
        catch(Exception e) { throw new IllegalArgumentException(e); }
        return out;
    }
    private static JSONObject envelope(String event, Object... pairs) throws Exception {
        JSONObject out=object(pairs);
        out.put("event",event); out.put("seq",++sequence);
        out.put("wall_ms",System.currentTimeMillis());
        out.put("elapsed_ns",SystemClock.elapsedRealtimeNanos());
        out.put("thread",Thread.currentThread().getName());
        return out;
    }
    public static synchronized void event(String event,Object... pairs) {
        try {
            String line=envelope(event,pairs).toString();
            Log.i("XrealProbe",line);
            if(events!=null) {
                if(new File(directory,"events.jsonl").length()>8*1024*1024) {events.close();rotate("events.jsonl");events=writer("events.jsonl");}
                events.write(line); events.newLine(); events.flush();
            }
        } catch(Exception e) { Log.e("XrealProbe","Journal write failed",e); }
    }
    static synchronized void pose(JSONObject sample) {
        try {
            if(new File(directory,"poses.jsonl").length()>16*1024*1024) {poses.close();rotate("poses.jsonl");poses=writer("poses.jsonl");}
            poses.write(envelope("pose_query","sample",sample).toString()); poses.newLine(); poses.flush();
        } catch(Exception e) { event("pose_write_error","error",e.toString()); }
    }
    static void latest(JSONObject status) {
        try {
            File temp=new File(directory,"latest.tmp");
            try(FileOutputStream out=new FileOutputStream(temp)) {out.write(status.toString(2).getBytes("UTF-8"));}
            if(!temp.renameTo(new File(directory,"latest.json"))) throw new IOException("rename latest");
        } catch(Exception e) { event("latest_write_error","error",e.toString()); }
    }
    public static void stopCapture() { if(logcat!=null) logcat.destroy(); }
}
