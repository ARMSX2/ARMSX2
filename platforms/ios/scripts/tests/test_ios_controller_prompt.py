"""App dialogs go through RootView's prompt window, which a controller can answer."""

import re
import unittest

from ios_source import SWIFT, block, read, without_comments


VIEWS = SWIFT / "Views"
ROOT = VIEWS / "RootView.swift"

# Still native on purpose: these already switch to a controller alert themselves, ask for text,
# or live in the touch-only layout editor.
NATIVE = {
    "PerGameSettingsPanel.swift", "CheatsPatchesManagerView.swift", "GameListView.swift",
    "BIOSListView.swift", "StorageSettingsView.swift", "VirtualPadSettingsView.swift",
    "PadLayoutEditView.swift",
}


class ControllerPromptDialogs(unittest.TestCase):
    """Reset Appearance and Reset Audio asked through a native confirmation dialog, a
    UIAlertController that the controller cannot reach, so the pad moved the page behind it."""

    def test_only_the_listed_files_keep_native_dialogs(self):
        native = {
            path.name for path in VIEWS.rglob("*.swift")
            if re.search(r"\.(confirmationDialog|alert)\(", without_comments(read(path)))
        }
        self.assertEqual(native - NATIVE, set())

    def test_the_reset_button_asks_through_the_prompt(self):
        button = block(without_comments(read(VIEWS / "Settings/NumberRow.swift")),
                       "struct ConfirmedSettingsResetButton")
        self.assertIn("ControllerPrompt.shared.ask(", button)
        self.assertNotIn(".confirmationDialog(", button)

    def test_root_view_shows_and_answers_the_prompt(self):
        root = without_comments(read(ROOT))
        self.assertIn("if let request = ControllerPrompt.shared.request {", root)
        self.assertIn("return .prompt(request.id)", root)
        self.assertIn("ControllerPrompt.shared.answer(index)", root)
        self.assertIn("ControllerPrompt.shared.answer(nil)", root)


if __name__ == "__main__":
    unittest.main()
