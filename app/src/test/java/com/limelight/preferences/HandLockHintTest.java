package com.limelight.preferences;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/** Text describing the triple-pinch hand lock, in every language. */
public class HandLockHintTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };
    // Words for the removed padlock setting, per language.
    private static final String[] PADLOCK_WORDS = { "padlock", "cadenas", "图标", "圖示" };
    // The gesture does not use the ring finger.
    private static final String[] RING_WORDS = { "ring finger", "annulaire", "无名指", "無名指" };

    private static File file(String path) {
        File f = new File(path);
        return f.exists() ? f : new File("app/" + path);
    }

    private static String string(String dir, String name) throws Exception {
        String strings = new String(Files.readAllBytes(
                file("src/main/res/" + dir + "/strings.xml").toPath()), StandardCharsets.UTF_8);
        Matcher m = Pattern.compile("name=\"" + name + "\">([^<]+)<").matcher(strings);
        return m.find() ? m.group(1) : null;
    }

    @Test
    public void theHintIsWordedInEveryLanguage() throws Exception {
        for (String dir : LANGUAGES) {
            for (String name : new String[] { "vr_hand_hint_title", "vr_hand_hint_body",
                    "vr_hand_hint_never" }) {
                assertNotNull(dir + " " + name, string(dir, name));
            }
        }
        assertEquals("Hand tracking is on.", string("values", "vr_hand_hint_title"));
        assertEquals("Pinch your thumb and index finger three times quickly to lock your hands"
                + " out, so a stray pinch does not click; the same triple pinch unlocks them.",
                string("values", "vr_hand_hint_body"));
        assertEquals("Don\\'t show this again", string("values", "vr_hand_hint_never"));
    }
}
