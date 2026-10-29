package com.limelight;

import android.app.Application;
import android.preference.PreferenceManager;

import com.limelight.binding.video.DepthSelfTest;
import com.limelight.preferences.PreferenceConfiguration;

/**
 * Starts the file log and runs the preference migrations. The log setting is read straight
 * from shared preferences, since PreferenceConfiguration does display and codec lookups.
 */
public class MoonlightApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();

        String setting = PreferenceManager.getDefaultSharedPreferences(this)
                .getString(PreferenceConfiguration.FILE_LOG_PREF_STRING,
                           PreferenceConfiguration.DEFAULT_FILE_LOG);
        FileLog.init(this, FileLog.levelFromName(setting));

        // Before any activity applies the xml defaults. The depth rate move goes before the Gen 1
        // seed, which writes a rate to the same key.
        PreferenceConfiguration.migrateDepthRate(this);
        PreferenceConfiguration.seedGen1PerfProfile(this);
        PreferenceConfiguration.migrateDepthSource(this);

        // Debug builds only, and only when its properties are set
        DepthSelfTest.startIfAsked(this);
    }
}
