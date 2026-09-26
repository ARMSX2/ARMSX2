"""A preset's shaders count cannot crash Settings > Shaders."""

import unittest

from ios_source import SWIFT, block, read, without_comments


class ShaderPassCount(unittest.TestCase):
    """shaders = -1 made 0..<count trap, and the preset reference is saved, so Settings >
    Shaders crashed every time it opened."""

    def test_the_count_is_bounded_before_the_range(self):
        inspect = block(without_comments(read(SWIFT / "Models/ShaderPassLibrary.swift")),
                        "nonisolated static func inspect(")
        self.assertLess(inspect.index("(0...64).contains($0)"), inspect.index("Array(0..<$0)"),
                        "the shaders count reaches 0..<count unchecked")


if __name__ == "__main__":
    unittest.main()
