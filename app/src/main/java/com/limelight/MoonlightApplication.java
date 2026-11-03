package com.limelight;

import android.app.Application;

import com.limelight.binding.video.DepthSelfTest;

/** Starts the file log, at warnings and errors. */
public class MoonlightApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();

        FileLog.init(this, FileLog.BASIC);

        // Debug builds only, and only when its properties are set
        DepthSelfTest.startIfAsked(this);
    }
}
