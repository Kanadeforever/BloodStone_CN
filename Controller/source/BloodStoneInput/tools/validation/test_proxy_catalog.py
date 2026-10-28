#!/usr/bin/env python3
"""校验七个正式代理、默认 DirectInput 策略及不发行 version.dll 的一致性。"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ProxyCatalogTests(unittest.TestCase):
    def test_seven_proxy_targets_and_packager(self) -> None:
        # CMake 目标与发行复制清单必须同步，杜绝把无效入口打包给用户。
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        builder = (ROOT / 'tools/build_release.py').read_text(encoding='utf-8')
        expected = ('xinput1_1', 'xinput1_2', 'xinput1_3', 'xinput1_4',
                    'xinput9_1_0', 'dinput8', 'winmm')
        targets = re.findall(r'^joyshim_add_proxy\(\w+ "([^"]+)" \w+\)', cmake, re.M)
        self.assertEqual(tuple(targets), expected)
        package = re.search(r'PROXIES = \((.*?)\)', builder, re.S)
        self.assertIsNotNone(package)
        self.assertEqual(tuple(re.findall(r"'([^']+)'", package.group(1))), expected)
        self.assertIn('if (stage / arch / "version.dll").exists()', builder)

    def test_version_entry_is_not_available(self) -> None:
        # 禁止退役入口被重新注册或随包附带导出表。
        for name in ('exports_version_x86.def', 'exports_version_x64.def'):
            self.assertFalse((ROOT / 'src' / name).exists())
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        entry = (ROOT / 'src/proxy_entry.cpp').read_text(encoding='utf-8')
        self.assertNotIn('JoyShimVersion', cmake)
        self.assertNotIn('ProxyEntryKind::Version', entry)

    def test_no_config_keeps_directinput_visible(self) -> None:
        # 文件不存在、文件配置未指定、配置开关关闭三种情况都要默认放行。
        hdr = (ROOT / 'src/config.hpp').read_text(encoding='utf-8')
        cpp = (ROOT / 'src/config.cpp').read_text(encoding='utf-8')
        self.assertEqual(hdr.count('bool blockDirectInput{false};'), 2)
        self.assertIn('bool enabled = false;', cpp)
        template = (ROOT / 'src/default_config.hpp').read_text(encoding='utf-8')
        self.assertIn('HideDirectInput = false', template)
        self.assertIn('assert(!ConfigRequestsDirectInputInterception(defaults));',
                      (ROOT / 'tests/test_config_scope.cpp').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
