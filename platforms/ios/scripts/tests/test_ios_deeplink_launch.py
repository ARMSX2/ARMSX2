#!/usr/bin/env python3
"""A launch link's filename is decoded once, and the link's shape is the published one.

`queryValue` returns a value that is already percent-decoded on both of its paths. Decoding
it a second time reads a literal % in the filename as the start of a new escape: `100%.iso`
percent-encodes to `100%25.iso`, the first decode gives `100%.iso`, and the second sees `%`
followed by `.i`, which is not hex, so `removingPercentEncoding` returns nil and the whole
guard fails. The player is then told the link is missing a filename it plainly carries.

The rest of this file pins the format the app publishes for itself. `libraryPayload` hands
every game a `launchURL` that other frontends store and replay later, so the verbs and the
parameter names are a contract with software this repository does not control.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
HANDLER = ROOT / "platforms/ios/app/src/main/swift/Models/ARMSX2DeepLinkHandler.swift"
INFO = ROOT / "platforms/ios/app/src/main/cpp/Info.plist.in"

LAUNCH_VERBS = ("launch", "boot", "play")
GAME_KEYS = ('"game"', '"iso"', '"file"', '"name"')
SCHEMES = ("armsx2", "armsx2-ios", "armsx2ios")


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
        """libraryPayload hands this string to other apps, which replay it much later."""
        published = re.search(r'let launchURL = "([^"]+)"', self.source)
        self.assertIsNotNone(published, "libraryPayload no longer publishes a launchURL")
        template = published.group(1)
        self.assertTrue(template.startswith("armsx2://"), "published launchURL is not armsx2://")
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
            self.source, r'let launchURL = "[^"]*\\\(percentEncoded\(',
            "the published launchURL interpolates the filename raw, so a name with a space "
            "or an ampersand produces a link that cannot be parsed back")

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
