package kr.co.iefriends.pcsx2.utils;

import static org.junit.Assert.assertEquals;

import kr.co.iefriends.pcsx2.utils.BackPressPolicy.Action;

import org.junit.Test;

/**
 * Regression coverage for the in-game system back button decision logic.
 *
 * The Activity turns a back press into one of three actions; these tests pin the full
 * transition table so the "double-back to exit" timing and the older exit behaviour
 * cannot silently regress.
 */
public class BackPressPolicyTest {

    private static final long WINDOW_MS = 2000L;
    private static final long NOW = 1_000_000L;

    // --- Core transitions ---

    @Test
    public void closedAndDisarmed_opensMenuAndArms() {
        assertEquals(Action.OPEN_MENU_ARM,
            BackPressPolicy.decide(false, 0L, NOW, WINDOW_MS));
    }

    @Test
    public void armed_secondBackImmediately_exits() {
        assertEquals(Action.EXIT,
            BackPressPolicy.decide(true, NOW, NOW, WINDOW_MS));
    }

    @Test
    public void armed_secondBackJustInsideWindow_exits() {
        assertEquals(Action.EXIT,
            BackPressPolicy.decide(true, NOW, NOW + 1999, WINDOW_MS));
    }

    @Test
    public void armed_secondBackAtWindowEdge_exits() {
        assertEquals(Action.EXIT,
            BackPressPolicy.decide(true, NOW, NOW + WINDOW_MS, WINDOW_MS));
    }

    @Test
    public void windowExpired_menuOpen_closesMenu() {
        assertEquals(Action.CLOSE_MENU,
            BackPressPolicy.decide(true, NOW, NOW + WINDOW_MS + 1, WINDOW_MS));
    }

    @Test
    public void windowExpired_menuClosed_reopensMenu() {
        assertEquals(Action.OPEN_MENU_ARM,
            BackPressPolicy.decide(false, NOW, NOW + WINDOW_MS + 1, WINDOW_MS));
    }

    // --- Regression guards ---

    @Test
    public void openedViaToggleButton_notArmed_closesMenuNotExit() {
        // Menu opened by the floating toggle (never armed by a back press).
        assertEquals(Action.CLOSE_MENU,
            BackPressPolicy.decide(true, 0L, NOW, WINDOW_MS));
    }

    @Test
    public void staleArmFromEarlier_treatedAsDisarmed() {
        assertEquals(Action.OPEN_MENU_ARM,
            BackPressPolicy.decide(false, NOW, NOW + 10_000_000L, WINDOW_MS));
    }

    @Test
    public void zeroTimestampNeverArmed_evenAtClockZero() {
        assertEquals(Action.OPEN_MENU_ARM,
            BackPressPolicy.decide(false, 0L, 0L, WINDOW_MS));
    }
}
