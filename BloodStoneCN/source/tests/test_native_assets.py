# -*- coding: utf-8 -*-
"""验证新 ASI 的支持版本、安装布局和不可变的原始资源边界。"""

from __future__ import annotations

import importlib.util
import json
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'source'
OUTPUT = ROOT / 'release' / 'Runtime'


class NativeAssetsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.code = (SOURCE / 'src/bloodstone_cn.cpp').read_text(encoding='utf-8')
        cls.profile = json.loads((SOURCE / 'config/profile.json').read_text(encoding='utf-8'))

    def test_exactly_five_dump_verified_entrypoints(self) -> None:
        # 每个补丁站点必须来自原始明文转储分析，不允许用随机匹配替代验证。
        self.assertEqual(len(self.profile['sites']), 5)
        self.assertEqual({item['name'] for item in self.profile['sites']},
                         {'loc_common', 'loc_levels', 'loc_multiplayer', 'font_resource', 'font_dat'})
        for item in self.profile['sites']:
            value = int(item['instruction_rva'], 16)
            self.assertIn(f'0x{value:08X}ul', self.code)
            self.assertEqual(bytes.fromhex(item['instruction_bytes'])[0], 0x68)
            self.assertEqual(struct.unpack('<I', bytes.fromhex(item['instruction_bytes'])[1:])[0],
                             int(item['target_va'], 16))

    def test_original_code_and_path_signatures_required(self) -> None:
        # 正式插件不能只根据内存地址盲目覆盖，必须同时核对原始代码和资源名。
        self.assertIn('ValidateImage(module)', self.code)
        self.assertIn('Matches(reinterpret_cast<const char*>', self.code)
        self.assertIn('instruction[0] != 0x68', self.code)
        self.assertIn('ChangePointer(', self.code)
        self.assertIn('State::WaitingForDecryption', self.code)
        self.assertIn('State::PatchFailed', self.code)

    def test_game_language_and_text_hooks_untouched(self) -> None:
        # 模块不需要替换 GetText、SubmitText、NativeGlyph 或全局语言变量。
        self.assertNotIn('0x00E87740', self.code)
        self.assertNotIn('0x00E85900', self.code)
        self.assertNotIn('0x0142C1B0', self.code)
        self.assertIn('mode=EN_UNCHANGED pak=ORIGINAL text_hooks=0 glyph_hooks=0', self.code)

    def test_public_runtime_has_no_prebuilt_game_assets(self) -> None:
        # 用户的可安装成品由 toolkit build 放入 release/output/Runtime。
        # 公开 Runtime 只提供程序，不能重新带入任何 LOC、DAT 或 BPR。
        root = OUTPUT / 'BSCNContents'
        for suffix in ('*.loc', '*.dat', '*.bpr', '*.bfa', '*.pak'):
            self.assertFalse(list(root.rglob(suffix)))

    def test_no_legacy_active_asi_and_no_repacked_pak(self) -> None:
        # 用户已经否定重封 PAK，正式安装目录不能同时携带旧字体 ASI。
        self.assertTrue((OUTPUT / 'BloodstoneCN.asi').is_file())
        self.assertFalse((OUTPUT / 'EurLooseRedirect.asi').exists())
        self.assertFalse((ROOT / 'release' / '候选封包' / 'cache' / 'gamedata.pak').exists())

    def test_result_log_tracks_plugin_directory(self) -> None:
        # 只有用户通过命令行指定 --log 时，才写入 ASI 同目录的诊断文件。
        self.assertIn('g_plugin_dir', self.code)
        self.assertIn('BloodstoneCN.log', self.code)
        self.assertIn('HasLogArgument(GetCommandLineW())', self.code)
        self.assertIn('path_mode=', self.code)
        self.assertIn('legacy_modules_font_text=', self.code)
        self.assertIn('FIVE_PATHS_REDIRECTED', self.code)

    def test_any_asi_directory_including_unicode_attempt(self) -> None:
        # 实际目录必须来自当前 DLL，不能依赖固定的 scripts 或 plugins 文件夹。
        self.assertIn('GetDirectory(self, g_plugin_dir)', self.code)
        self.assertIn('EncodeNativePath(g_plugin_dir', self.code)
        self.assertIn('EncodeNativePath(g_plugin_dir + base', self.code)
        self.assertIn('WideCharToMultiByte', self.code)
        self.assertIn('GetShortPathNameW', self.code)
        self.assertIn('AppendFormatSafe(g_loc_common', self.code)
        self.assertIn('AppendFormatSafe(g_loc_levels', self.code)
        self.assertIn('AppendFormatSafe(g_loc_multiplayer', self.code)
        self.assertNotIn('ASI_OUTSIDE_GAME_ROOT', self.code)
        self.assertNotIn('LEGACY_ASI_CONFLICT', self.code)

    def test_legacy_asi_is_diagnosed_not_preemptively_rejected(self) -> None:
        # 只记录旧模块是否装载；真正的冲突要由被修改的原始字符串、指令判定。
        self.assertIn('legacy_modules_font_text=', self.code)
        self.assertIn('State::ForeignPatchConflict', self.code)
        self.assertIn('FOREIGN_CODE_PATCH_DETECTED', self.code)
        self.assertNotIn('return State::LegacyConflict', self.code)
        self.assertIn('patch_count=', self.code)
        self.assertIn('failed_site_1_to_5=', self.code)

    def test_actual_exe_dump_if_available(self) -> None:
        # 安装者不会随发行包收到游戏 EXE。开发者可把明文转储路径放在环境变量中，
        # 测试会额外验证真实二进制，而没有转储时仍用随包精确签名验证。
        import os
        location = os.environ.get('BSCN_BOND_UNPACKED')
        if not location:
            self.skipTest('未提供游戏 EXE 转储路径，跳过可选的真实二进制复核')
        spec = importlib.util.spec_from_file_location('dump_verifier', SOURCE / 'tools/verify_dump.py')
        self.assertIsNotNone(spec.loader)
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        result = verifier.verify_dump(Path(location).read_bytes(), self.profile)
        self.assertEqual(len(result['verified']), 5)

    def test_bat_encoding(self) -> None:
        for path in (SOURCE / 'build.bat', ROOT / 'toolbox/start.bat'):
            raw = path.read_bytes()
            self.assertFalse(raw.startswith(b'\xef\xbb\xbf'))
            self.assertNotIn(b'\n', raw.replace(b'\r\n', b''))
            for line in raw.decode('utf-8').splitlines():
                if line.strip().lower().startswith(('rem ', 'echo ')):
                    self.assertTrue(line.endswith('  '), (path.name, line))


if __name__ == '__main__':
    unittest.main()
