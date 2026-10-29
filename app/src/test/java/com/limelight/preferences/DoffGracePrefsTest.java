package com.limelight.preferences;

import android.content.SharedPreferences;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/** The doff grace setting (hold for a minute after the headset comes off). */
public class DoffGracePrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };

    @Test
    public void itIsOnUnlessTurnedOff() {
        assertEquals("checkbox_vr_doff_grace", PreferenceConfiguration.VR_DOFF_GRACE_PREF_STRING);
        assertTrue(PreferenceConfiguration.DEFAULT_VR_DOFF_GRACE);
        FakePrefs prefs = new FakePrefs();
        assertTrue(PreferenceConfiguration.doffGraceOn(prefs));
        prefs.values.put(PreferenceConfiguration.VR_DOFF_GRACE_PREF_STRING, false);
        assertFalse(PreferenceConfiguration.doffGraceOn(prefs));
        prefs.values.put(PreferenceConfiguration.VR_DOFF_GRACE_PREF_STRING, true);
        assertTrue(PreferenceConfiguration.doffGraceOn(prefs));
    }

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    private static org.w3c.dom.Document parse(String path) throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        return factory.newDocumentBuilder().parse(res(path));
    }

    private static String strings(String dir) throws Exception {
        return new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                StandardCharsets.UTF_8);
    }

    private static String string(String strings, String key) {
        Matcher m = Pattern.compile("name=\"" + key + "\">([^<]+)<").matcher(strings);
        return m.find() ? m.group(1) : null;
    }

    private static final class FakePrefs implements SharedPreferences {
        final Map<String, Object> values = new HashMap<>();

        @Override public Map<String, ?> getAll() { return values; }
        @Override public String getString(String key, String def) { return def; }
        @Override public Set<String> getStringSet(String key, Set<String> def) { return def; }
        @Override public int getInt(String key, int def) { return def; }
        @Override public long getLong(String key, long def) { return def; }
        @Override public float getFloat(String key, float def) { return def; }
        @Override public boolean getBoolean(String key, boolean def) {
            return values.containsKey(key) ? (Boolean)values.get(key) : def;
        }
        @Override public boolean contains(String key) { return values.containsKey(key); }
        @Override public Editor edit() { return null; }
        @Override public void registerOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }
        @Override public void unregisterOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }
    }
}
