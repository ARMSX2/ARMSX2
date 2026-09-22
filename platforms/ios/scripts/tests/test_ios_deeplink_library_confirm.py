"""An inbound link cannot send the library until someone approves the destination.

armsx2 is a public scheme, so the callback in a library link is chosen by whoever
opened it. exportLibrary used to build the payload and hand it straight to
UIApplication.shared.open, which put every game name, serial, region, CRC and file
size on a stranger's URL with nothing on screen. The export is split in two now:
the link parks its callback, and RootView asks before the second half runs.
"""

import unittest

from ios_source import CPP, SWIFT, at, block, without_comments


HANDLER = SWIFT / "Models/ARMSX2DeepLinkHandler.swift"
APP_STATE = SWIFT / "Models/AppState.swift"
ROOT_VIEW = SWIFT / "Views/RootView.swift"
SCENE = CPP / "IOS/SceneDelegate.mm"

PARK = "AppState.shared.pendingLibraryExport = callback"
OPEN = "UIApplication.shared.open"
SLOT = "var pendingLibraryExport: String?"
GATE = "appState.pendingLibraryExport != nil"
PERFORM = "performLibraryExport"


class DeepLinkLibraryExportConfirmation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.handler = without_comments(HANDLER.read_text(encoding="utf-8"))
        cls.app_state = without_comments(APP_STATE.read_text(encoding="utf-8"))
        cls.root = without_comments(ROOT_VIEW.read_text(encoding="utf-8"))
        cls.scene = SCENE.read_text(encoding="utf-8")

    def test_the_export_is_still_reachable_from_a_link(self):
        """Every check below passes on a handler that no longer exports at all."""
        self.assertIn(
            "exportLibrary(from url: URL)", self.handler,
            "ARMSX2DeepLinkHandler no longer routes a library link anywhere")
        self.assertIn(
            "DeepLinkBridge", self.scene,
            "SceneDelegate no longer hands incoming URLs to DeepLinkBridge, so nothing "
            "below is on a path a link can take")

    def test_the_link_parks_the_callback_instead_of_opening_it(self):
        body = block(self.handler, "static func exportLibrary(from url: URL)")
        self.assertIn(
            PARK, body,
            "exportLibrary does not park the callback on AppState, so nothing gives "
            "RootView a destination to ask about")
        self.assertNotIn(
            OPEN, body,
            "exportLibrary still opens the callback itself. A link reaches this without "
            "any interaction, so the library leaves the device before anyone is asked")

    def test_the_second_half_is_the_only_thing_that_opens_it(self):
        body = block(self.handler, "static func %s(callback: String)" % PERFORM)
        self.assertIn(
            OPEN, body,
            "%s no longer opens the callback, so confirming does nothing" % PERFORM)
        self.assertEqual(
            self.handler.count(OPEN), 1,
            "ARMSX2DeepLinkHandler opens a URL in more than one place. Every route out "
            "has to pass the prompt, so there can only be the one")

    def test_the_parked_callback_has_somewhere_to_sit(self):
        self.assertIn(
            SLOT, self.app_state,
            "AppState has no pendingLibraryExport, so the handler cannot park a callback "
            "and RootView has nothing to watch")

    def test_the_prompt_names_the_destination_and_offers_a_way_out(self):
        window = self.root[at(self.root, GATE, "RootView"):][:1400]
        self.assertIn(
            PERFORM, window,
            "the alert bound to pendingLibraryExport never calls %s, so confirming it "
            "sends nothing" % PERFORM)
        self.assertIn(
            ".host", window,
            "the alert does not name the callback's host. A prompt that hides who is "
            "receiving the list is not a decision anyone can make")
        self.assertIn(
            "role: .cancel", window,
            "the alert has no cancel role, so there is no way to refuse it")

    def test_nothing_else_calls_the_second_half(self):
        call = "ARMSX2DeepLinkHandler." + PERFORM + "("
        callers = sum(
            without_comments(path.read_text(encoding="utf-8")).count(call)
            for path in sorted(SWIFT.rglob("*.swift")))
        self.assertEqual(
            callers, 1,
            "%s is called from %d places in Swift, expected only the alert. A second "
            "caller is a second route around the prompt" % (call, callers))


if __name__ == "__main__":
    unittest.main()
