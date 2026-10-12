package org.shadps4.xrealprobe;

import android.Manifest;
import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.os.*;
import android.provider.MediaStore;
import android.view.*;
import android.widget.*;
import org.json.*;
import spatial.xreal.*;
import java.io.*;
import java.util.zip.*;

/** Disposable diagnostic host of the same asynchronous Foundation tracker as the emulator. */
public final class ProbeActivity extends Activity {
    private final Handler main=new Handler(Looper.getMainLooper());
    private XrealPoseTracker tracker;
    private TextView view;
    private boolean foreground, enabled=true;
    private int mode;
    private volatile long samples, usable;
    private volatile boolean sourceActive;
    private volatile String lastSample="{}";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        try {ProbeLog.init(this);} catch(Exception e) {throw new RuntimeException(e);}
        mode=getIntent().getIntExtra("tracking",0)==1?1:0;
        ProbeLog.event("host","model",Build.MODEL,"sdk",Build.VERSION.SDK_INT,"requested_tracking",mode,
            "adapter","Foundation XrealPoseTracker dedicated worker");
        LinearLayout layout=new LinearLayout(this);layout.setOrientation(LinearLayout.VERTICAL);layout.setPadding(18,12,18,12);
        LinearLayout buttons=new LinearLayout(this);
        button(buttons,"Resume",()->{enabled=true;update();});
        button(buttons,"Pause",()->{enabled=false;update();});
        button(buttons,"Export logs",this::exportLogs);
        button(buttons,"Stop",this::stop);
        layout.addView(buttons);
        TextView note=new TextView(this);
        note.setText("Foreground probe only. SDK calls run on XrealPose; UI never waits. Start with --ei tracking 1 for 3DoF (default 6DoF). Stop and reopen to retry SDK failures.\nNative, hardware, timing and raw pose journals are in this run's sdk/ directory.");
        layout.addView(note);
        ScrollView scroll=new ScrollView(this);view=new TextView(this);view.setTextSize(14);
        view.setTypeface(android.graphics.Typeface.MONOSPACE);scroll.addView(view);layout.addView(scroll);
        setContentView(layout);
        tracker=new XrealPoseTracker(new File(ProbeLog.directory,"sdk"),
            mode==0?XrealSession.TrackingMode.SIX_DOF:XrealSession.TrackingMode.THREE_DOF,
            new XrealPoseTracker.Listener() {
                public void sourceActive(boolean active) {sourceActive=active; if(!active)lastSample="{}";}
                public void pose(PoseSample sample,long queryStartedNs) {
                    samples++;if(sample.poseConfirmed)usable++;
                    JSONObject json=sample.toJson();lastSample=json.toString();
                    ProbeLog.pose(json);
                }
            });
    }
    private void button(LinearLayout parent,String text,Runnable action) {
        Button button=new Button(this);button.setText(text);button.setTextSize(11);
        button.setOnClickListener(v->action.run());parent.addView(button);
    }
    private void update() {
        if(tracker==null)return;
        tracker.setActive(foreground&&enabled?this:null,()->{
            if(checkSelfPermission(Manifest.permission.CAMERA)!=PackageManager.PERMISSION_GRANTED)
                requestPermissions(new String[]{Manifest.permission.CAMERA},7);
        });
    }
    private final Runnable render=new Runnable() {public void run() {
        if(!foreground)return;
        view.setText("Mode: "+(mode==0?"6DoF":"3DoF")+" | enabled="+enabled+" source="+sourceActive+
            "\nQueries="+samples+" usable="+usable+"\nRun: "+ProbeLog.directory.getName()+
            "\nQuery clock is NOT a hardware sample timestamp.\n\n"+lastSample);
        main.postDelayed(this,1000);
    }};
    private void exportLogs() {
        new Thread(()->{
            android.net.Uri uri=null;
            try {
                ContentValues values=new ContentValues();values.put(MediaStore.Downloads.DISPLAY_NAME,"xreal-probe-"+ProbeLog.directory.getName()+".zip");
                values.put(MediaStore.Downloads.MIME_TYPE,"application/zip");values.put(MediaStore.Downloads.IS_PENDING,1);
                uri=getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI,values);
                if(uri==null)throw new IOException("No download URI");
                try(ZipOutputStream zip=new ZipOutputStream(getContentResolver().openOutputStream(uri))) {zipDirectory(zip,ProbeLog.directory,"");}
                values.clear();values.put(MediaStore.Downloads.IS_PENDING,0);getContentResolver().update(uri,values,null,null);
                ProbeLog.event("export_complete","uri",uri.toString());
                runOnUiThread(()->Toast.makeText(this,"Saved ZIP in Downloads",Toast.LENGTH_LONG).show());
            }catch(Exception e){if(uri!=null)getContentResolver().delete(uri,null,null);ProbeLog.event("export_failed","error",e.toString());}
        },"probe-export").start();
    }
    private static void zipDirectory(ZipOutputStream zip,File directory,String prefix)throws IOException {
        File[] files=directory.listFiles();if(files==null)return;
        for(File file:files) {
            String name=prefix+file.getName();
            if(file.isDirectory()) {zipDirectory(zip,file,name+"/");continue;}
            if(name.endsWith(".tmp"))continue;
            zip.putNextEntry(new ZipEntry(name));try(InputStream in=new FileInputStream(file)){in.transferTo(zip);}zip.closeEntry();
        }
    }
    private void stop() {
        enabled=false;update();main.removeCallbacks(render);
        ProbeLog.stopCapture();finishAndRemoveTask();
        // Standalone diagnostic process only. The embedding emulator must never kill itself.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
    @Override public void onBackPressed(){stop();}
    @Override protected void onResume(){super.onResume();foreground=true;update();main.post(render);}
    @Override protected void onPause(){foreground=false;update();main.removeCallbacks(render);super.onPause();}
    @Override protected void onDestroy(){foreground=false;update();main.removeCallbacks(render);super.onDestroy();}
}
