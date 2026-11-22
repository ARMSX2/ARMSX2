"""The game menu's cover colours can be turned off."""

import unittest

from ios_source import block, read, swift, without_comments


STORE = swift("SettingsStoreAppearance.swift")
COORDINATOR = swift("GameLibraryControllerCoordinator.swift")
APPEARANCE = swift("AppearanceSettingsView.swift")
INTERFACE = swift("AppearanceInterfaceSections.swift")


class GameMenuCoverColours(unittest.TestCase):
    """Opening Game Options re-tints the menu from the cover art. Some players like it and
    some don't, so it has its own switch, on by default."""

    def test_it_is_on_unless_saved_off(self):
        source = without_comments(read(STORE))
        self.assertIn('forKey: "ARMSX2iOSGameMenuCoverColoursEnabled"\n            ) as? Bool ?? true', source)

    def test_the_game_menu_preview_checks_it(self):
        update = block(without_comments(read(COORDINATOR)), "func updateGameCoverThemePreview(")
        self.assertIn("? settings.appearance.gameMenuCoverColoursEnabled", update)
        self.assertIn(": settings.appearance.favoriteGlowingEffectEnabled", update)

    def test_appearance_offers_it_to_the_pad(self):
        source = without_comments(read(APPEARANCE) + read(INTERFACE))
        self.assertIn("isOn: Bindable(settings.appearance).gameMenuCoverColoursEnabled", source)
        self.assertEqual(source.count('"settings.appearance.game-menu-cover-colours"'), 2)


if __name__ == "__main__":
    unittest.main()
