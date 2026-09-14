#!/usr/bin/env python3
"""A launch link's filename is decoded once, and the link's shape is the published one.

`queryValue` returns a value that is already percent-decoded on both of its paths. Decoding
it a second time reads a literal % in the filename as the start of a new escape: `100%.iso`
percent-encodes to `100%25.iso`, the first decode gives `100%.iso`, and the second sees `%`
followed by `.i`, which is not hex, so `removingPercentEncoding` returns nil and the whole
guard fails. The player is then told the link is missing a filename it plainly carries.

The rest of this file pins the format the app publishes for itself. `libraryPayload` and the
game menu's Copy Launch Link hand out a `launchURL` that other frontends store and replay
later, so the verbs and the parameter names are a contract with software this repository
does not control.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
HANDLER = ROOT / "platforms/ios/app/src/main/swift/Models/ARMSX2DeepLinkHandler.swift"
GAME_LIST = ROOT / "platforms/ios/app/src/main/swift/Views/GameListView.swift"
INFO = ROOT / "platforms/ios/app/src/main/cpp/Info.plist.in"

LAUNCH_VERBS = ("launch", "boot", "play")
GAME_KEYS = ('"game"', '"iso"', '"file"', '"name"')
SCHEMES = ("armsx2", "armsx2-ios", "armsx2ios")
QUERY_SEPARATORS = set("&=+")


def body(source, name):
    """The text of one private static func, up to the next one at the same indent."""
    start = source.find("    private static func %s(" % name)
    if start < 0:
        return None
    following = source.find("\n    private static func ", start + 1)
    return source[start:following if following > 0 else len(source)]


class DeepLinkLaunch(unittest.TestCase):
    def setUp(self):
        self.assertTrue(HANDLER.is_file(), "missing %s" % HANDLER)
        self.source = HANDLER.read_text(encoding="utf-8")

    def test_the_filename_is_not_decoded_a_second_time(self):
        launch = body(self.source, "launchGame")
        self.assertIsNotNone(launch, "launchGame disappeared")
        self.assertIn("queryValue(", launch, "launchGame no longer reads the query")
        self.assertNotIn(
            "removingPercentEncoding", launch,
            "launchGame decodes the filename again. queryValue already returns it decoded, "
            "so a name holding a literal % is read as a broken escape and refused with a "
            "message about a missing filename.")

    def test_the_raw_query_fallback_still_decodes_once(self):
        """The fallback parses url.query by hand, so it is the one path that must decode."""
        fallback = body(self.source, "queryValue")
        self.assertIsNotNone(fallback, "queryValue disappeared")
        self.assertEqual(
            fallback.count("removingPercentEncoding"), 2,
            "the raw-query fallback decodes exactly its key and its value. A different count "
            "means either a decode was dropped, and an encoded name stops matching, or one "
            "was added, and the double-decode bug is back one level down.")

    def test_the_published_launch_url_matches_what_the_handler_accepts(self):
        """libraryPayload and the game menu hand this string to other apps, which replay it
        much later."""
        templates = re.findall(r'"(armsx2://[^"]*)"', self.source)
        self.assertEqual(
            len(templates), 1,
            "launch links are built in more than one place, so the copied link and the "
            "exported one can drift apart")
        self.assertIn(
            '"launchURL": launchURL(forISO:', self.source,
            "libraryPayload no longer publishes the shared launchURL")
        template = templates[0]
        verb = template.split("://", 1)[1].split("?", 1)[0].strip("/").lower()
        self.assertIn(
            verb, LAUNCH_VERBS,
            "the published launch URL uses a verb the handler does not route, so every link "
            "the app has ever handed out stops working")
        key = re.search(r"[?&](\w+)=", template)
        self.assertIsNotNone(key, "the published launchURL carries no parameter")
        self.assertIn(
            '"%s"' % key.group(1), GAME_KEYS,
            "the published launch URL names its parameter something launchGame does not read")

    def test_the_published_launch_url_percent_encodes_the_name(self):
        self.assertRegex(
            self.source, r'"armsx2://[^"]*\\\(percentEncoded\(',
            "the published launchURL interpolates the filename raw, so a name with a space "
            "or an ampersand produces a link that cannot be parsed back")

    def test_the_encoding_escapes_the_query_separators(self):
        """urlQueryAllowed leaves & = and + alone, so Ratchet & Clank.iso would come back as
        game=Ratchet with the rest of the name read as a second parameter."""
        encoder = body(self.source, "percentEncoded")
        self.assertIsNotNone(encoder, "percentEncoded disappeared")
        removed = re.search(r'remove\(charactersIn: "([^"]*)"\)', encoder)
        self.assertIsNotNone(
            removed,
            "percentEncoded uses urlQueryAllowed as it comes, which lets & = and + through, "
            "so a name holding one is cut short when the link is read back")
        missing = QUERY_SEPARATORS - set(removed.group(1))
        self.assertFalse(
            missing, "percentEncoded still lets %s through unescaped" % " ".join(sorted(missing)))

    def test_a_game_from_an_external_folder_boots_from_its_path(self):
        """availableISOs only knows games stored inside the app. A game from an external folder
        is listed by its filename but boots by path, the way tapping it in the library does."""
        launch = body(self.source, "launchGame")
        self.assertIsNotNone(launch, "launchGame disappeared")
        self.assertIn("bootName(forListedName:", launch, "launchGame no longer resolves the name")

        lookup = body(self.source, "bootName")
        self.assertIsNotNone(lookup, "bootName disappeared")
        local = lookup.find("availableISOs()")
        listed = lookup.find("availableISOEntries()")
        self.assertGreaterEqual(local, 0, "launch links stopped checking the app's own games")
        self.assertGreater(
            listed, local,
            "launch links no longer fall back to the full library after the app's own games, "
            "so a link copied from an external folder's game says it cannot be found")
        self.assertIn('"path"', lookup, "a game from an external folder no longer boots by path")

    def test_the_game_menu_copies_the_handlers_link(self):
        menu = GAME_LIST.read_text(encoding="utf-8")
        self.assertIn(
            "ARMSX2DeepLinkHandler.launchURL(forISO:", menu,
            "Copy Launch Link no longer asks the handler for the link")
        self.assertNotIn(
            '"armsx2://', menu,
            "the game list builds a launch link by hand, which can drift from what the "
            "handler accepts")

    def test_the_accepted_schemes_and_the_registered_ones_are_the_same_set(self):
        """Both directions. A scheme in code but not in the plist is the quiet one: iOS never
        routes the URL, so the handler that would accept it is never reached and nothing
        anywhere reports a problem."""
        plist = INFO.read_text(encoding="utf-8")
        declared = re.search(r"supportedSchemes[^\]]*\]", self.source)
        self.assertIsNotNone(declared, "supportedSchemes disappeared")
        accepted = set(re.findall(r'"([^"]+)"', declared.group(0)))

        block = re.search(
            r"<key>CFBundleURLSchemes</key>\s*<array>(.*?)</array>", plist, re.S)
        self.assertIsNotNone(block, "Info.plist registers no CFBundleURLSchemes")
        registered = set(re.findall(r"<string>([^<]+)</string>", block.group(1)))

        self.assertEqual(
            accepted, registered,
            "the schemes the handler accepts and the schemes Info.plist registers have "
            "drifted apart. In code but not in the plist means iOS never hands the app the "
            "URL; in the plist but not in code means the app claims a scheme it refuses.")
        self.assertEqual(accepted, set(SCHEMES), "the published scheme list changed")


if __name__ == "__main__":
    unittest.main()
