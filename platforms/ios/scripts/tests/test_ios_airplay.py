import plistlib
import unittest

from ios_source import IOS_ROOT, RUNTIME_DIR, SWIFT, swift


class AirPlayTests(unittest.TestCase):
    def test_external_display_owns_render_while_phone_keeps_controls(self):
        manifest = plistlib.loads((IOS_ROOT / "Info.plist.in").read_bytes())["UIApplicationSceneManifest"]
        configurations = manifest["UISceneConfigurations"]
        self.assertFalse(manifest["UIApplicationSupportsMultipleScenes"])

        header = (RUNTIME_DIR / "PCSX2SceneDelegate.h").read_text()
        scene_delegate = (RUNTIME_DIR / "SceneDelegate.mm").read_text()
        expected_scenes = {
            "UIWindowSceneSessionRoleApplication": ("Default Configuration", "PCSX2SceneDelegate"),
            "UIWindowSceneSessionRoleExternalDisplayNonInteractive": (
                "External Display", "ARMSX2ExternalDisplaySceneDelegate"
            ),
        }
        for role, (name, delegate) in expected_scenes.items():
            with self.subTest(role=role):
                configuration = configurations[role][0]
                self.assertEqual(configuration["UISceneConfigurationName"], name)
                self.assertEqual(configuration["UISceneDelegateClassName"], delegate)
                self.assertIn(
                    f"@interface {delegate} : UIResponder <UIWindowSceneDelegate", header
                )
                self.assertIn(f"@implementation {delegate}", scene_delegate)
        self.assertIn("Runtime/SceneDelegate.mm", (IOS_ROOT / "CMakeLists.txt").read_text())

        game_screen = swift("GameScreenView.swift").read_text()
        metal_view = swift("MetalGameView.swift").read_text()
        app_delegate = (RUNTIME_DIR / "AppDelegate.mm").read_text()
        self.assertIn("PhoneGameSurface()", game_screen)
        self.assertNotIn("MetalGameView()", game_screen)
        self.assertIn("if appState.externalDisplayConnected", metal_view)
        self.assertIn("if case .playing = appState.currentScreen", metal_view)
        self.assertIn('settings.localized("Please select a game")', metal_view)

        self.assertIn("isEqualToString:UIWindowSceneSessionRoleExternalDisplayNonInteractive", app_delegate)
        self.assertIn('configurationWithName:(external ? @"External Display" : @"Default Configuration")', app_delegate)
        self.assertIn("sessionRole:connectingSceneSession.role", app_delegate)

        self.assertIn(
            "self.window = [[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene] autorelease];",
            scene_delegate.split("@implementation ARMSX2ExternalDisplaySceneDelegate", 1)[1],
        )

    def test_scene_lookups_never_land_on_the_tv(self):
        walkers = [
            path.relative_to(SWIFT).as_posix()
            for path in sorted(SWIFT.rglob("*.swift"))
            if "connectedScenes" in path.read_text()
        ]
        self.assertEqual(walkers, ["App/SwiftUIHost.swift"])
        self.assertIn(
            "connectedScenes.first { $0.session.role == .windowApplication }",
            swift("SwiftUIHost.swift").read_text(),
        )


if __name__ == "__main__":
    unittest.main()
