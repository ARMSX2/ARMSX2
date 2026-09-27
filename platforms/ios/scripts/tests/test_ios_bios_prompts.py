"""The BIOS page's Replace and Restart prompts take controller input on their own."""

import unittest

from ios_source import SWIFT, block, read, without_comments


BIOS = SWIFT / "Views/BIOSListView.swift"


class BIOSPromptsOwnTheirInput(unittest.TestCase):
    """Only the page's library listener drove these prompts, so moving the page onto the
    shared navigation would have left them without a way to answer."""

    @classmethod
    def setUpClass(cls):
        cls.bios = without_comments(read(BIOS))

    def test_a_prompt_claims_its_own_capture(self):
        capture = block(self.bios, "private func updatePromptCapture(")
        self.assertIn("MenuControllerNavigationCaptureOwner.biosPrompt", capture)
        self.assertIn("activePrompt != nil", capture)

    def test_its_listener_only_takes_that_owner(self):
        listener = block(self.bios, "private struct BIOSPromptCommandListener")
        self.assertIn("== MenuControllerNavigationCaptureOwner.biosPrompt", listener)


if __name__ == "__main__":
    unittest.main()
