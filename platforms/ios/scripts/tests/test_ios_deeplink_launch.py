#!/usr/bin/env python3
"""A launch link's filename is decoded once, and the link's shape is the published one.

queryValue already returns a decoded value, so decoding it again refuses a name like `100%.iso`.
libraryPayload and Copy Launch Link hand out a launchURL that other frontends store.
"""

import re
import unittest

from ios_source import CPP, ROOT, SWIFT

HANDLER = SWIFT / "Models/ARMSX2DeepLinkHandler.swift"
GAME_LIST = SWIFT / "Views/GameListView.swift"
APP_STATE = SWIFT / "Models/AppState.swift"
ROOT_VIEW = SWIFT / "Views/RootView.swift"
INFO = CPP / "Info.plist.in"

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
            "launchGame decodes the filename that queryValue already decoded")

    def test_the_raw_query_fallback_still_decodes_once(self):
        """The fallback parses url.query by hand, so it is the one path that must decode."""
        fallback = body(self.source, "queryValue")
        self.assertIsNotNone(fallback, "queryValue disappeared")
        self.assertEqual(
            fallback.count("removingPercentEncoding"), 2,
            "queryValue's raw-query fallback does not decode exactly its key and its value")

    def test_the_published_launch_url_matches_what_the_handler_accepts(self):
        """libraryPayload and the game menu hand this string to other apps, which store it."""
        templates = re.findall(r'"(armsx2://[^"]*)"', self.source)
        self.assertEqual(
            len(templates), 1,
            "ARMSX2DeepLinkHandler builds armsx2:// links in more than one place")
        self.assertIn(
            '"launchURL": launchURL(forISO:', self.source,
            "libraryPayload no longer publishes the shared launchURL")
        template = templates[0]
        verb = template.split("://", 1)[1].split("?", 1)[0].strip("/").lower()
        self.assertIn(
            verb, LAUNCH_VERBS,
            "the published launch URL uses a verb the handler does not route")
        key = re.search(r"[?&](\w+)=", template)
        self.assertIsNotNone(key, "the published launchURL carries no parameter")
        self.assertIn(
            '"%s"' % key.group(1), GAME_KEYS,
            "the published launch URL names its parameter something launchGame does not read")

    def test_the_published_launch_url_percent_encodes_the_name(self):
        self.assertRegex(
            self.source, r'"armsx2://[^"]*\\\(percentEncoded\(',
            "the published launchURL interpolates the filename without percentEncoded(")

    def test_the_encoding_escapes_the_query_separators(self):
        """urlQueryAllowed leaves & = and + alone, so Ratchet & Clank.iso would come back as
        game=Ratchet with the rest of the name read as a second parameter."""
        encoder = body(self.source, "percentEncoded")
        self.assertIsNotNone(encoder, "percentEncoded disappeared")
        removed = re.search(r'remove\(charactersIn: "([^"]*)"\)', encoder)
        self.assertIsNotNone(
            removed,
            "percentEncoded does not remove & = and + from urlQueryAllowed")
        missing = QUERY_SEPARATORS - set(removed.group(1))
        self.assertFalse(
            missing, "percentEncoded lets %s through unescaped" % " ".join(sorted(missing)))

    def test_links_and_the_export_read_one_game_list(self):
        """Links and the export both read listedGames, which covers external folders."""
        launch = body(self.source, "launchGame")
        self.assertIsNotNone(launch, "launchGame disappeared")
        self.assertIn("bootName(forListedName:", launch, "launchGame no longer resolves the name")

        listed = body(self.source, "listedGames")
        self.assertIsNotNone(listed, "listedGames disappeared")
        self.assertIn(
            "availableISOEntries()", listed,
            "the game list no longer reads the entries, so games in external folders drop out")
        self.assertRegex(
            listed, r"external\s*\?\s*path\s*:\s*name",
            "a game from an external folder no longer boots by its path")
        for reader in ("bootName", "libraryPayload"):
            self.assertIn(
                "listedGames()", body(self.source, reader) or "",
                "%s no longer reads listedGames, so links and the export can disagree about which "
                "games exist" % reader)

    def test_the_game_menu_copies_the_handlers_link(self):
        menu = GAME_LIST.read_text(encoding="utf-8")
        self.assertIn(
            "ARMSX2DeepLinkHandler.launchURL(forISO:", menu,
            "Copy Launch Link no longer asks the handler for the link")
        self.assertNotIn(
            '"armsx2://', menu,
            "the game list builds a launch link by hand, which can drift from what the "
            "handler accepts")

    def test_a_link_asks_before_replacing_a_running_game(self):
        """Booting under a live VM rewrites its settings and breaks its disc reads, so bootGame
        hands a running game to the Restart VM prompt instead of booting over it."""
        state = APP_STATE.read_text(encoding="utf-8")
        start = state.find("    func bootGame(")
        self.assertGreaterEqual(start, 0, "bootGame disappeared")
        boot = state[start:state.find("\n    }\n", start)]
        guard = boot.find("runningGameName != nil")
        self.assertGreaterEqual(guard, 0, "bootGame no longer checks for a running game")
        self.assertLess(
            guard, boot.find("performBootGame("),
            "bootGame can reach performBootGame before it checks for a running game")
        self.assertIn("pendingRestartGame = isoName", boot, "a running game no longer raises the prompt")
        self.assertIn(
            "shutdownAndBoot(isoName:", ROOT_VIEW.read_text(encoding="utf-8"),
            "the Restart VM prompt no longer restarts into the linked game")

    def test_files_opened_in_the_app_fall_back_to_the_importer(self):
        """The app runs on UIKit scenes, so SwiftUI's onOpenURL never fires. Every URL arrives
        through DeepLinkBridge, which has to pass anything that is not a link to the importer."""
        bridge = self.source[self.source.find("class DeepLinkBridge"):]
        self.assertIn(
            "FileImportHandler.shared.handleURL(url)", bridge,
            "DeepLinkBridge does not pass URLs that are not armsx2 links to FileImportHandler")
        self.assertNotIn(
            ".onOpenURL", ROOT_VIEW.read_text(encoding="utf-8"),
            "RootView has an onOpenURL, which never runs under the UIKit scene lifecycle")

    def test_the_launch_link_page_names_what_the_handler_accepts(self):
        """Frontend authors read platforms/ios/docs/launch-links.md, not this handler."""
        page = (ROOT / "platforms/ios/docs/launch-links.md").read_text(encoding="utf-8")
        for word in SCHEMES + LAUNCH_VERBS + tuple(key.strip('"') for key in GAME_KEYS):
            self.assertIn(
                "`%s`" % word, page,
                "launch-links.md no longer names `%s`, which the handler accepts" % word)

    def test_the_accepted_schemes_and_the_registered_ones_are_the_same_set(self):
        """supportedSchemes and the CFBundleURLSchemes in Info.plist list the same schemes."""
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
            "supportedSchemes and the CFBundleURLSchemes in Info.plist list different schemes")
        self.assertEqual(accepted, set(SCHEMES), "the published scheme list changed")


if __name__ == "__main__":
    unittest.main()
