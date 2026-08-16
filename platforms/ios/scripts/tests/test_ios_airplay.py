import plistlib
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
IOS = ROOT / "platforms/ios/app/src/main"


class AirPlayTests(unittest.TestCase):
    def test_external_display_owns_render_while_phone_keeps_controls(self):
        with (IOS / "cpp/Info.plist.in").open("rb") as source:
            manifest = plistlib.load(source)["UIApplicationSceneManifest"]
        configurations = manifest["UISceneConfigurations"]
        role = "UIWindowSceneSessionRoleExternalDisplayNonInteractive"
        self.assertEqual(
            configurations[role][0]["UISceneDelegateClassName"],
            "ARMSX2ExternalDisplaySceneDelegate",
        )

        game_screen = (IOS / "swift/Views/GameScreenView.swift").read_text()
        metal_view = (IOS / "swift/Views/MetalGameView.swift").read_text()
        game_list = (IOS / "swift/Views/GameListView.swift").read_text()
        app_delegate = (IOS / "cpp/IOS/AppDelegate.mm").read_text()
        self.assertIn("PhoneGameSurface()", game_screen)
        self.assertIn("if appState.externalDisplayConnected", metal_view)
        self.assertIn("if case .playing = appState.currentScreen", metal_view)
        self.assertIn('Text("Please select a game")', metal_view)
        self.assertIn('external ? @"External Display" : @"Default Configuration"', app_delegate)
        self.assertGreaterEqual(game_list.count(".focusable()"), 3)


if __name__ == "__main__":
    unittest.main()
