package com.limelight.preferences;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.Node;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/** The old app name appears in no language's strings. */
public class AboutPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };

    private static File file(String path) {
        File f = new File(path);
        return f.exists() ? f : new File("app/" + path);
    }

    private static File res(String path) {
        return file("src/main/res/" + path);
    }

    @Test
    public void theOldNameIsInNoLanguage() throws Exception {
        File[] dirs = res("").listFiles((f) -> f.getName().startsWith("values"));
        assertNotNull(dirs);
        for (File dir : dirs) {
            File strings = new File(dir, "strings.xml");
            if (!strings.isFile()) {
                continue;
            }
            String text = new String(Files.readAllBytes(strings.toPath()), StandardCharsets.UTF_8);
            assertFalse(dir.getName(), text.matches("(?is).*moonlight ?xr.*"));
        }
    }

}
