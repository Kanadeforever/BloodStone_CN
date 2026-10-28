#!/usr/bin/env python3
"""检查 Windows 诊断工具的入口及 MinGW CRT 配置匹配，避免链接时 WinMain 未定义。"""

from pathlib import Path
import re
import unittest

PROJECT = Path(__file__).resolve().parents[2]
CMAKE = (PROJECT / "CMakeLists.txt").read_text(encoding="utf-8")
SOURCES = PROJECT / "tools" / "src"
WIDE = ("JoyShimGamepadProbe", "JoyShimGamepadInfo", "JoyShimConfigCheck",
        "JoyShimProxyEntryProbe")
NARROW = ("JoyShimXInputStateProbe",)
NAMES = {
    "JoyShimGamepadProbe": "gamepad_probe.cpp",
    "JoyShimGamepadInfo": "gamepad_info.cpp",
    "JoyShimConfigCheck": "config_check.cpp",
    "JoyShimProxyEntryProbe": "proxy_entry_probe.cpp",
    "JoyShimXInputStateProbe": "xinput_state_probe.cpp",
}


class UnicodeEntrypointTests(unittest.TestCase):
    def test_source_entrypoints_match(self):
        # 检查目标的源文件确实采用宽字符或普通入口，防止仅更改构建脚本。
        for target in WIDE:
            source = (SOURCES / NAMES[target]).read_text(encoding="utf-8")
            self.assertRegex(source, r"\bint\s+wmain\s*\(", target)
        for target in NARROW:
            source = (SOURCES / NAMES[target]).read_text(encoding="utf-8")
            self.assertRegex(source, r"\bint\s+main\s*\(", target)

    def test_mingw_unicode_linker_option(self):
        # -municode 只针对 MinGW 和宽字符入口，在 MSVC 中不应被传入。
        group = re.search(r"if\(MINGW\)\s+foreach\(target (.*?)\)\s+target_link_options\(\$\{target\} PRIVATE -municode\)",
                          CMAKE, re.DOTALL)
        self.assertIsNotNone(group)
        targets = set(group.group(1).split())
        self.assertEqual(targets, set(WIDE))
        self.assertTrue(targets.isdisjoint(NARROW))

    def test_static_proxy_probe_is_not_waiting_for_sdl_dll(self):
        # 静态 SDL3 不会出现在系统模块列表；不能沿用动态库存在性检测。
        probe = (SOURCES / NAMES["JoyShimProxyEntryProbe"]).read_text(encoding="utf-8")
        self.assertNotIn('WaitForSdl3(', probe)
        self.assertNotIn('GetModuleHandleW(L"SDL3.dll")', probe)
        self.assertIn("初始化状态不由入口探针判定", probe)


if __name__ == "__main__":
    unittest.main(verbosity=2)
