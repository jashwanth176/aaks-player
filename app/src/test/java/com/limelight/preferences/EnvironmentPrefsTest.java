package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.EnvironmentIds;
import com.limelight.binding.video.XrShared;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/** The environment list value versus the saved environment id and passthrough checkbox. */
public class EnvironmentPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };
    private static final String ID = PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING;
    private static final String PASSTHROUGH = PreferenceConfiguration.VR_PASSTHROUGH_PREF_STRING;
    private static final String LEGACY = PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING;
    // Ids the picker offers, in order.
    private static final int[] OFFERED = {
            PreferenceConfiguration.VR_ENV_PASSTHROUGH, PreferenceConfiguration.VR_ENV_VOID,
    };

    @Test
    public void thePickersChoiceIsWhatTheListShows() {
        // Once an id is saved, the passthrough checkbox is ignored.
        for (int id : OFFERED) {
            for (boolean passthrough : new boolean[] { false, true }) {
                FakePrefs prefs = new FakePrefs();
                prefs.putInt(ID, id);
                prefs.putBoolean(PASSTHROUGH, passthrough);
                assertEquals("id " + id, Integer.toString(id), EnvironmentIds.listValue(prefs));
                assertEquals(Integer.toString(id), EnvironmentIds.listValue(prefs, !passthrough));
            }
        }
    }

    @Test
    public void theListsChoiceIsWhatTheNextSessionOpensWith() {
        for (int id : OFFERED) {
            FakePrefs prefs = new FakePrefs();
            assertEquals(id, EnvironmentIds.idForListValue(Integer.toString(id)));
            assertTrue(EnvironmentIds.store(prefs, EnvironmentIds.idForListValue(Integer.toString(id))));
            // Same key and type the picker writes and the session reads.
            assertEquals(id, prefs.getInt(ID, -1));
            assertTrue(prefs.values.get(ID) instanceof Integer);
            assertFalse(prefs.contains(PreferenceConfiguration.VR_ENVIRONMENT_LIST_PREF_STRING));
            assertEquals(Integer.toString(id), EnvironmentIds.listValue(prefs));
        }
    }

    @Test
    public void thePassthroughCheckboxFollowsTheList() {
        // On for passthrough, off otherwise, as the picker sets it.
        FakePrefs prefs = new FakePrefs();
        EnvironmentIds.store(prefs, PreferenceConfiguration.VR_ENV_PASSTHROUGH);
        assertTrue(prefs.getBoolean(PASSTHROUGH, false));
        for (int id : OFFERED) {
            EnvironmentIds.store(prefs, id);
            assertEquals("id " + id, id == PreferenceConfiguration.VR_ENV_PASSTHROUGH,
                    prefs.getBoolean(PASSTHROUGH, !(id == PreferenceConfiguration.VR_ENV_PASSTHROUGH)));
        }
    }

    @Test
    public void neverPickedTheCheckboxStillDecides() {
        FakePrefs prefs = new FakePrefs();
        // Default: the void (1).
        assertEquals("1", EnvironmentIds.listValue(prefs));
        prefs.putBoolean(PASSTHROUGH, true);
        assertEquals("0", EnvironmentIds.listValue(prefs));
        prefs.putBoolean(PASSTHROUGH, false);
        assertEquals("1", EnvironmentIds.listValue(prefs));
        // A pending checkbox value overrides the stored one.
        assertEquals("0", EnvironmentIds.listValue(prefs, true));
        assertEquals("1", EnvironmentIds.listValue(prefs, false));
        // Reading writes nothing, so the checkbox keeps deciding
        assertFalse(prefs.contains(ID));
    }

    @Test
    public void anOldInstallsCellReadsAsTheSessionReadsIt() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(LEGACY, 0);
        assertEquals("0", EnvironmentIds.listValue(prefs, false));
        prefs.putInt(LEGACY, 1);
        assertEquals("1", EnvironmentIds.listValue(prefs, true));
        // Removed photos and rooms read as the void.
        for (int legacy = 2; legacy < 8; legacy++) {
            prefs.putInt(LEGACY, legacy);
            assertEquals("cell " + legacy, "1", EnvironmentIds.listValue(prefs, true));
        }
        // Outside the old layout: treated as unset.
        prefs.putInt(LEGACY, 8);
        assertEquals("0", EnvironmentIds.listValue(prefs, true));
        // A saved id takes precedence.
        prefs.putInt(ID, PreferenceConfiguration.VR_ENV_VOID);
        prefs.putInt(LEGACY, 0);
        assertEquals("1", EnvironmentIds.listValue(prefs, true));
        assertFalse(prefs.contains(PreferenceConfiguration.VR_ENVIRONMENT_LIST_PREF_STRING));
    }

    @Test
    public void aRetiredIdShowsAsTheVoid() {
        // Includes the removed rooms (4-6).
        for (int id : new int[] { 2, 3, 4, 5, 6, 7, 99, 100, 103 }) {
            FakePrefs prefs = new FakePrefs();
            prefs.putInt(ID, id);
            prefs.putBoolean(PASSTHROUGH, true);
            assertEquals("id " + id, "1", EnvironmentIds.listValue(prefs));
        }
    }

    @Test
    public void nothingElseIsWritten() {
        for (String value : new String[] { "2", "3", "7", "100", "-1", "", "void", null, "4.0" }) {
            FakePrefs prefs = new FakePrefs();
            prefs.putInt(ID, PreferenceConfiguration.VR_ENV_HOME_THEATER);
            prefs.putBoolean(PASSTHROUGH, true);
            assertEquals("value " + value, -1, EnvironmentIds.idForListValue(value));
            assertFalse(EnvironmentIds.store(prefs, EnvironmentIds.idForListValue(value)));
            assertEquals(PreferenceConfiguration.VR_ENV_HOME_THEATER, prefs.getInt(ID, -1));
            assertTrue(prefs.getBoolean(PASSTHROUGH, false));
        }
        assertEquals(1, EnvironmentIds.idForListValue(" 1 "));
        assertEquals(-1, EnvironmentIds.idForListValue("5"));
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        String english = strings("values");
        assertEquals("Passthrough", string(english, "vr_environment_passthrough"));
        assertEquals("Black void", string(english, "vr_environment_void"));
        assertNull(string(english, "vr_environment_home_theater"));
        for (String dir : LANGUAGES) {
            String strings = strings(dir);
            for (String key : new String[] { "vr_environment_passthrough", "vr_environment_void" }) {
                assertNotNull(dir + " " + key, string(strings, key));
            }
        }
        assertNull(string(english, "vr_environment_photo"));
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

    private static final class FakePrefs implements SharedPreferences, SharedPreferences.Editor {
        final Map<String, Object> values = new HashMap<>();

        @Override public Map<String, ?> getAll() { return values; }
        @Override public String getString(String key, String def) {
            return values.containsKey(key) ? (String)values.get(key) : def;
        }
        @Override public Set<String> getStringSet(String key, Set<String> def) { return def; }
        @Override public int getInt(String key, int def) {
            return values.containsKey(key) ? (Integer)values.get(key) : def;
        }
        @Override public long getLong(String key, long def) { return def; }
        @Override public float getFloat(String key, float def) { return def; }
        @Override public boolean getBoolean(String key, boolean def) {
            return values.containsKey(key) ? (Boolean)values.get(key) : def;
        }
        @Override public boolean contains(String key) { return values.containsKey(key); }
        @Override public Editor edit() { return this; }
        @Override public void registerOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }
        @Override public void unregisterOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }

        @Override public Editor putString(String key, String value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putStringSet(String key, Set<String> value) { return this; }
        @Override public Editor putInt(String key, int value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putLong(String key, long value) { return this; }
        @Override public Editor putFloat(String key, float value) { return this; }
        @Override public Editor putBoolean(String key, boolean value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor remove(String key) {
            values.remove(key);
            return this;
        }
        @Override public Editor clear() {
            values.clear();
            return this;
        }
        @Override public boolean commit() { return true; }
        @Override public void apply() { }
    }
}
