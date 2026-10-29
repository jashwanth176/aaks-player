package com.limelight.binding.video;

import com.limelight.preferences.PreferenceConfiguration;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class EnvironmentIdsTest {

    @Test
    public void savedIdsKeepTheirMeaning() {
        // Persisted ids; must never change.
        assertEquals(ENV_CELL_PASSTHROUGH, EnvironmentIds.cellForId(0));
        assertEquals(ENV_CELL_VOID, EnvironmentIds.cellForId(1));
        assertEquals(4, PreferenceConfiguration.VR_ENV_HOME_THEATER);
        assertEquals(5, PreferenceConfiguration.VR_ENV_GRAND_CINEMA);
        assertEquals(6, PreferenceConfiguration.VR_ENV_SYNTHWAVE);
    }

    @Test
    public void theRemovedRoomsComeUpInTheVoid() {
        // Removed rooms map to no cell and start in the void.
        for (int id : new int[] { 4, 5, 6 }) {
            assertEquals(-1, EnvironmentIds.cellForId(id));
            assertEquals(ENV_CELL_VOID, EnvironmentIds.startCell(id, true));
            assertEquals(ENV_CELL_VOID, EnvironmentIds.startCell(id, false));
        }
    }

    @Test
    public void theCellsRunInThePickersOrder() {
        assertEquals(0, ENV_CELL_PASSTHROUGH);
        assertEquals(1, ENV_CELL_VOID);
        assertEquals(2, ENV_CELL_COUNT);
    }

    @Test
    public void retiredIdsNameNothing() {
        // Retired ids are never reused.
        assertEquals(2, EnvironmentIds.RETIRED_MINIMAL_ROOM);
        assertEquals(3, EnvironmentIds.RETIRED_PSX_CINEMA);
        assertEquals(100, EnvironmentIds.RETIRED_FIRST_PHOTO);
        int[] retired = { 2, 3, 100, 101, 102, 103, 104 };
        for (int id : retired) {
            assertEquals("id " + id, -1, EnvironmentIds.cellForId(id));
        }
        for (int cell = 0; cell < ENV_CELL_COUNT; cell++) {
            int id = EnvironmentIds.idForCell(cell);
            assertTrue("cell " + cell, id != 2 && id != 3 && id < 100);
        }
    }

    @Test
    public void aRetiredIdStartsInTheVoid() {
        // The passthrough checkbox is ignored: the user picked something else.
        int[] retired = { 2, 3, 100, 101, 102, 103, 104, 99, 7 };
        for (int id : retired) {
            assertEquals("id " + id, ENV_CELL_VOID, EnvironmentIds.startCell(id, true));
            assertEquals("id " + id, ENV_CELL_VOID, EnvironmentIds.startCell(id, false));
        }
    }

    @Test
    public void neverPickedFollowsThePassthroughCheckbox() {
        assertEquals(ENV_CELL_PASSTHROUGH, EnvironmentIds.startCell(-1, true));
        assertEquals(ENV_CELL_VOID, EnvironmentIds.startCell(-1, false));
    }

    @Test
    public void savedIdsStartWhereTheyWereLeft() {
        for (int cell = 0; cell < ENV_CELL_COUNT; cell++) {
            int id = EnvironmentIds.idForCell(cell);
            assertEquals("cell " + cell, cell, EnvironmentIds.startCell(id, false));
            assertEquals("cell " + cell, cell, EnvironmentIds.startCell(id, true));
        }
    }

    @Test
    public void everyCellRoundTrips() {
        for (int cell = 0; cell < ENV_CELL_COUNT; cell++) {
            int id = EnvironmentIds.idForCell(cell);
            assertTrue("cell " + cell, id >= 0);
            assertEquals("cell " + cell, cell, EnvironmentIds.cellForId(id));
        }
    }

    @Test
    public void unknownIdsAndCellsMapToNothing() {
        assertEquals(-1, EnvironmentIds.cellForId(-1));
        assertEquals(-1, EnvironmentIds.cellForId(99));
        assertEquals(-1, EnvironmentIds.idForCell(-1));
        assertEquals(-1, EnvironmentIds.idForCell(ENV_CELL_COUNT));
        assertEquals(-1, EnvironmentIds.idForCell(PICKER_CELLS));
    }

    @Test
    public void legacyCellsReadAsWhatTheyMeantThen() {
        // Old cell order: passthrough, void, four photos, two rooms.
        int[] expectedIds = {
                PreferenceConfiguration.VR_ENV_PASSTHROUGH, PreferenceConfiguration.VR_ENV_VOID,
                100, 101, 102, 103, 2, 3,
        };
        for (int legacy = 0; legacy < expectedIds.length; legacy++) {
            assertEquals("legacy " + legacy, expectedIds[legacy],
                    EnvironmentIds.idForLegacyCell(legacy));
        }
        assertEquals(ENV_CELL_PASSTHROUGH,
                EnvironmentIds.startCell(EnvironmentIds.idForLegacyCell(0), false));
        assertEquals(ENV_CELL_VOID,
                EnvironmentIds.startCell(EnvironmentIds.idForLegacyCell(1), true));
        for (int legacy = 2; legacy < expectedIds.length; legacy++) {
            assertEquals("legacy " + legacy, ENV_CELL_VOID,
                    EnvironmentIds.startCell(EnvironmentIds.idForLegacyCell(legacy), true));
        }
        assertEquals(-1, EnvironmentIds.idForLegacyCell(-1));
        assertEquals(-1, EnvironmentIds.idForLegacyCell(expectedIds.length));
    }

    @Test
    public void noCellIsARoom() {
        for (int cell = 0; cell < ENV_CELL_COUNT; cell++) {
            assertFalse(EnvironmentIds.isRoomCell(cell));
        }
        assertFalse(PreferenceConfiguration.isRoomEnvironment(PreferenceConfiguration.VR_ENV_HOME_THEATER));
    }

    @Test
    public void theGridFitsItsCells() {
        // One row of two square cells, no blank tile.
        assertEquals(1, PICKER_ROWS);
        assertEquals(2, PICKER_COLS);
        assertEquals(PICKER_CELL_PX * PICKER_COLS, PICKER_TEX_W);
        assertEquals(PICKER_HEADER_PX + PICKER_CELL_PX, PICKER_TEX_H);
        assertEquals(PICKER_CELLS, ENV_CELL_COUNT);
    }
}
