"""Square opens the game menu and Triangle adds a favorite."""

import unittest

from ios_source import SWIFT, read


ROUTER = SWIFT / "Models/MenuControllerInputRouter.swift"


class SquareTriangle(unittest.TestCase):
    """Testers asked for Square on the game menu and Triangle on favorites."""

    def test_face_buttons(self):
        source = read(ROUTER)
        self.assertIn("gamepad.buttonX.isPressed, command: .showContextMenu", source)
        self.assertIn("gamepad.buttonY.isPressed, command: .toggleFavorite", source)


if __name__ == "__main__":
    unittest.main()
