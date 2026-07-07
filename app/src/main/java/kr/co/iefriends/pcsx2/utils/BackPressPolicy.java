package kr.co.iefriends.pcsx2.utils;

/**
 * Pure decision logic for the in-game system back button.
 *
 * Kept free of any Android dependencies so the behaviour can be unit-tested in isolation.
 * The hosting Activity supplies the current state and performs the resulting side effect.
 */
public final class BackPressPolicy {

    private BackPressPolicy() {}

    public enum Action {
        /** Exit the running game and return to the library. */
        EXIT,
        /** Close the in-game options menu. */
        CLOSE_MENU,
        /** Open the in-game options menu and arm the double-back exit shortcut. */
        OPEN_MENU_ARM
    }

    /**
     * Decide what a system back press should do while a game is running.
     *
     * @param drawerOpen    whether the in-game options drawer is currently open
     * @param lastPromptMs  timestamp (ms) of the back press that armed the exit shortcut, or 0 if disarmed
     * @param nowMs         current timestamp (ms)
     * @param windowMs      how long the exit shortcut stays armed after opening the menu
     */
    public static Action decide(boolean drawerOpen, long lastPromptMs, long nowMs, long windowMs) {
        boolean exitArmed = lastPromptMs != 0L && (nowMs - lastPromptMs) <= windowMs;
        if (exitArmed) {
            return Action.EXIT;
        }
        if (drawerOpen) {
            return Action.CLOSE_MENU;
        }
        return Action.OPEN_MENU_ARM;
    }
}
