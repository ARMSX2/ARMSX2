# test_ios_pad_layout_set_aside.py — an unreadable VPadLayouts.json is moved aside, never saved over
# SPDX-License-Identifier: GPL-3.0+

import re
import unittest

from ios_source import block, read, swift, without_comments


def store():
    return without_comments(read(swift("PadLayoutPresetStore.swift")))


def body(text, needle):
    """The statements of a function, without its signature and outer braces."""
    found = block(text, needle)
    return found[found.index("{") + 1:-1].strip()


class UnreadableLayoutsFile(unittest.TestCase):
    """A file that failed to decode used to empty the store, and the next persist()
    wrote the empty library over it, so every saved layout was lost."""

    def unreadable_branch(self):
        init = block(store(), "    init(\n        libraryURL: URL? = nil")
        return init[init.index("case .corrupt:"):]

    def test_the_file_is_moved_to_an_unreadable_name(self):
        branch = self.unreadable_branch()
        self.assertRegex(branch, r"FileManager\.default\.moveItem\(at: (self\.)?libraryURL, to:")
        self.assertIn('"unreadable-', branch)

    def test_a_failed_move_makes_the_store_read_only(self):
        moved, failed = self.unreadable_branch().split("} catch {", 1)
        self.assertIn("readOnly = true", failed)
        self.assertNotIn("readOnly = true", moved)

    def test_a_moved_file_migrates_like_a_missing_one(self):
        branch = self.unreadable_branch()
        migrate = re.search(r"if migrateFromCurrentLayout \{\s*migrateCurrentLayout\(currentLayout\)", branch)
        self.assertIsNotNone(migrate)
        self.assertLess(branch.index("moveItem"), migrate.start())

    def test_persist_and_save_write_nothing_when_read_only(self):
        text = store()
        self.assertTrue(body(text, "private func persist()").startswith("guard !readOnly else { return }"))
        self.assertTrue(body(text, "func save() throws").startswith(
            "guard !readOnly else { throw PadLayoutPresetStoreError.readOnly }"))


class LoadingWritesNothing(unittest.TestCase):
    """@Observable runs didSet in init, so setting globalPresetID while loading saved the
    file before the game assignments were read and every per-game layout pick was lost."""

    def test_the_loaded_branch_bypasses_the_persisting_setter(self):
        init = block(store(), "    init(\n        libraryURL: URL? = nil")
        branch = init[init.index("case .loaded"):init.index("case .missing:")]
        self.assertIn("_globalPresetID = validPresetID(library.globalPresetID)", branch)
        self.assertNotRegex(branch, r"(?<!_)\bglobalPresetID =")
        self.assertNotIn("persist()", branch)


if __name__ == "__main__":
    unittest.main()
