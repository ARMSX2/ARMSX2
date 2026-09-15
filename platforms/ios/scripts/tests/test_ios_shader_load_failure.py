#!/usr/bin/env python3
"""The Shaders section says why a shader preset can't load, using librashader's error."""

import re
import unittest

from ios_source import CPP, SWIFT, at, block, read

BRIDGE = CPP / "ARMSX2Bridge.mm"
PARAMS = SWIFT / "Models/ShaderParams.swift"
IMPORTER = SWIFT / "Models/ShaderPackImporter.swift"
SECTION = SWIFT / "Views/Settings/ShaderChainSection.swift"


class ShaderPresetLoadFailure(unittest.TestCase):
    def test_both_bridge_failures_pass_the_error_on(self):
        body = re.search(
            r"(?ms)^\+ \(nullable NSString \*\)shaderPresetParametersAtPath:.*?\n\}\n",
            read(BRIDGE))
        self.assertIsNotNone(body, "shaderPresetParametersAtPath: is gone")
        self.assertEqual(
            body.group(0).count("ARMSX2ShaderPresetFailure(err, error);"), 2,
            "a failure branch frees librashader's error instead of handing it to Swift")

    def test_only_a_missing_base_pack_file_asks_for_the_base_pack(self):
        self.assertRegex(
            read(PARAMS),
            r"path\.contains\(base\) && !ShaderPresetLibrary\.hasBasePack",
            "the base pack prompt no longer checks both the missing path and whether the pack is "
            "there")

    def test_the_importer_keeps_a_pack_whose_presets_fail(self):
        text = read(IMPORTER)
        guard = at(text, "guard !presets.isEmpty", "the empty pack guard")
        check = at(text, "problem(in: presets)", "the preset check")
        self.assertLess(guard, check, "presets are parsed before an empty pack is refused")
        start = at(text, "func problem(in presets:", "the preset check body")
        body = text[start:text.find("\n    }\n", start)]
        self.assertIn("ARMSX2Bridge.shaderPresetParameters(atPath:", body,
                      "the import check no longer parses a preset")
        self.assertNotIn("removeItem", body,
                         "a pack is deleted because its presets fail")

    def test_the_reason_shows_under_preset(self):
        text = read(SECTION)
        preset = at(text, 'Text(localized("Preset"))', "the Preset row")
        failure = at(text, "params.loadFailure", "the load failure line")
        download = at(text, 'localized("Download Shaders")', "the Download row")
        self.assertLess(preset, failure, "the reason sits above the Preset row it explains")
        self.assertLess(failure, download,
                        "the reason sits below Download Shaders, away from the Preset row")

    def test_an_older_parse_cannot_land_on_a_newer_preset(self):
        load = block(read(PARAMS), "func load(token newToken: String)")
        cleared = at(load, "params = []", "clearing the previous preset's rows")
        parse = at(load, "Task.detached", "the parse")
        check = at(load, "guard current == generation else { return }", "the generation check")
        push = at(load, "pushEffective()", "the push")
        self.assertLess(cleared, parse, "the previous preset's rows stay listed while the next one is read")
        self.assertLess(parse, check, "the generation check runs before the parse, so it misses a newer load")
        self.assertLess(check, push, "a parse that a newer load overtook still pushes its values")

    def test_an_unresolvable_preset_stops_loading(self):
        load = block(read(PARAMS), "func load(token newToken: String)")
        unresolved = block(load, "guard let url = ShaderPresetLibrary.resolve(newToken) else")
        self.assertIn("isLoading = false", unresolved,
                      "a token that names no file leaves Reading parameters on screen")

    def test_a_path_that_leaves_the_shader_roots_asks_for_a_reinstall(self):
        init = block(read(PARAMS), "init(_ error: Error, preset: URL)")
        self.assertIn('path.hasPrefix("/private/")', init,
                      "a device path under /private/var is never compared with the /var shader roots")
        self.assertIn("ShaderPresetLibrary.token(for:", init,
                      "the reinstall check doesn't test the path against the shader roots")
        self.assertLess(at(init, 'path.contains("/../")', "the relative climb check"),
                        at(init, "path.contains(base) && !ShaderPresetLibrary.hasBasePack",
                           "the base pack check"),
                        "an old-layout pack is offered the base pack download before the reinstall note")


if __name__ == "__main__":
    unittest.main()
