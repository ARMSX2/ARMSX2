"""PerGameSettingsPanel keeps no per-game value in @State; PerGameSettingsDraft owns them."""

import re
import unittest

from ios_source import read, swift, without_comments


PANEL = swift("PerGameSettingsPanel.swift")
DRAFT = swift("PerGameSettingsDraft.swift")
STATE = re.compile(r"@State\s+(?:private\s+)?var\s+(\w+)")
MEMBER = re.compile(r"^    (?:let|var)\s+(\w+)\s*:", re.M)


class PanelHoldsNoDraftState(unittest.TestCase):
    """The panel held 121 @State, 97 of them per-game values it loaded and saved itself."""

    @classmethod
    def setUpClass(cls):
        cls.panel = without_comments(read(PANEL))
        cls.states = set(STATE.findall(cls.panel))
        cls.draft = set(MEMBER.findall(without_comments(read(DRAFT))))

    def test_the_draft_values_were_found(self):
        self.assertGreater(len(self.draft), 90)

    def test_no_draft_value_is_panel_state(self):
        self.assertEqual(self.states & self.draft, set())

    def test_no_per_game_or_global_state(self):
        self.assertEqual({s for s in self.states if re.match(r"(perGame|global)[A-Z]", s)}, set())

    def test_the_panel_builds_its_draft(self):
        self.assertIn("State(initialValue: PerGameSettingsDraft(", self.panel)


if __name__ == "__main__":
    unittest.main()
