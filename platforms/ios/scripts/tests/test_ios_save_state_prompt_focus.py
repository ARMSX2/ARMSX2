"""The Load and Replace Last Saved State prompts show their focus ring on the confirm action at once."""

import unittest

from ios_source import SWIFT, read, without_comments


GAME = SWIFT / "Views/GameScreenView.swift"


class SaveStatePromptFocus(unittest.TestCase):
    """Testers opened Load Last Saved State with the controller macro and saw no focus ring until
    they pressed the D-pad, and Replace Last Saved State started on Cancel."""

    def setUp(self):
        source = without_comments(read(GAME))
        start = source.find("if let prompt = saveStateShortcutPrompt {")
        self.prompt = source[start:start + 2400]

    def test_the_prompt_enters_its_session_when_it_opens(self):
        self.assertIn(".task(id: prompt)", self.prompt)
        self.assertIn('matchingScopePrefix: "runtime.last-save-state-confirmation"', self.prompt)

    def test_focus_starts_on_the_action_after_cancel(self):
        self.assertIn("selectedIndex: 1,", self.prompt)
        self.assertIn("preferredInitialFocusLabel: saveStateShortcutPromptActions(prompt)[1].title", self.prompt)


if __name__ == "__main__":
    unittest.main()
