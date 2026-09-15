#!/usr/bin/env python3
"""A failed shader preset is built again after a pick, Shaders on, or the base pack download."""

import re
import unittest

from ios_source import ROOT, SWIFT, at, block, read

DEVICE = ROOT / "pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm"
COMMON = ROOT / "pcsx2/GS/Renderers/Common/GSDevice.cpp"
RETRY = "ARMSX2Bridge.retryShaderChain()"


class ShaderChainRetry(unittest.TestCase):
    def test_the_latch_lets_go_once_the_counter_moves(self):
        text = read(DEVICE)
        start = at(text, "bool GSDeviceMTL::DoApplyShaderChain", "DoApplyShaderChain")
        latch = at(text, "if (m_shader_chain_failed &&", "the failure latch", start)
        compare = at(text, "if (m_shader_chain_retry == GetShaderChainRetry())", "the retry check", start)
        destroy = at(text, "DestroyShaderChain();", "dropping the failed chain", compare)
        rebuild = at(text, "if (!m_shader_chain ||", "the rebuild", start)
        record = at(text, "m_shader_chain_retry = GetShaderChainRetry();", "recording the counter", start)
        load = at(text, "libra_preset_create(", "the preset load", start)
        self.assertLess(latch, compare, "the retry check sits outside the failure latch")
        self.assertLess(compare, rebuild,
                        "the failed preset is rebuilt before the retry counter is checked")
        self.assertLess(destroy, rebuild,
                        "a chain that failed mid-frame is not destroyed before the rebuild")
        self.assertLess(record, load,
                        "the retry counter is recorded after libra_preset_create")

    def test_the_counter_moves(self):
        self.assertIn("s_shader_chain_retry.fetch_add(1",
                      block(read(COMMON), "void GSDevice::RetryShaderChain()"),
                      "RetryShaderChain doesn't move the counter")

    def test_every_way_back_to_a_preset_retries_it(self):
        store = read(SWIFT / "Models/SettingsStore.swift")
        self.assertIn(RETRY, block(store, "private func applyShaderChainSelection()"),
                      "picking a preset in Settings or the pause menu doesn't retry it")
        self.assertIn(RETRY, block(store, "var shaderChainEnabled: Bool"),
                      "turning Shaders on doesn't retry the preset")
        self.assertRegex(
            read(SWIFT / "Views/PerGameSettingsPanel.swift"),
            r"perGameShaderPresetRef = token\s+shaderPresetRequest = nil\s+" + re.escape(RETRY),
            "picking a per-game preset doesn't retry it")
        self.assertIn(RETRY, block(read(SWIFT / "Models/PerGameShaderSelection.swift"), "static func write(chain:"),
                      "saving per-game shaders doesn't retry the preset")
        section = read(SWIFT / "Views/Settings/ShaderChainSection.swift")
        self.assertLess(at(section, "await importer.installBasePack()", "the base pack download"),
                        at(section, RETRY, "the retry after it"),
                        "a preset waiting on RetroArch Slang Shaders isn't retried once they arrive")


if __name__ == "__main__":
    unittest.main()
