# -*- coding: utf-8 -*-
"""核对原版随机字母扰动的精确分支签名，防止将中文渐显修复误用到其他版本。"""
from __future__ import annotations

import importlib.util
import json
import os
import struct
import unittest
from pathlib import Path

BASE = Path(__file__).resolve().parents[1]
PROFILE = BASE / 'config/profile.json'
SOURCE = BASE / 'src/bloodstone_cn.cpp'
VERIFY = BASE / 'tools/verify_dump.py'


class TextEffectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 测试使用与实际 ASI 一致的 profile，不能另外写一份容易失效的地址表。
        cls.profile = json.loads(PROFILE.read_text(encoding='utf-8'))
        cls.effect = cls.profile['text_effect']
        cls.code = SOURCE.read_text(encoding='utf-8')
        spec = importlib.util.spec_from_file_location('bscn_effect_verifier', VERIFY)
        cls.verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.verifier)

    def test_four_branches_have_exact_mapping(self):
        # 第三项是随机字母，其余三个分支必须原样保留。
        targets = [int(x, 16) for x in self.effect['original_targets']]
        self.assertEqual(targets, [0xC6994E, 0xC69976, 0xC69A5C, 0xC69B3E])
        self.assertEqual(int(self.effect['disabled_target_va'], 16), 0xC69BB6)
        self.assertEqual(int(self.effect['jump_table_va'], 16), 0xC69C74)

    def test_random_clamp_is_ascii_z(self):
        # 原版将临时字符上限固定为字母 z，汉字参与计算时会退化成 Z/z。
        self.assertTrue(bytes.fromhex(self.effect['ascii_clamp_bytes']).endswith(b'\x7a'))
        self.assertEqual(bytes.fromhex(self.effect['random_branch_bytes'])[:5], b'\xb8\x01\x00\x00\x00')

    def test_only_table_third_entry_is_written(self):
        # 修改跳转表项而不是整段文本更新函数，仍保留显示进度与其他效果。
        self.assertIn('module + kEffectJumpTableRva', self.code)
        self.assertIn('reinterpret_cast<volatile unsigned long*>(table + 2)', self.code)
        self.assertIn('kEffectSkipRva', self.code)
        self.assertIn('DISABLED_RANDOM_LETTERS', self.code)
        self.assertNotIn('GetTextHook', self.code)

    def test_strict_signatures_and_rollback(self):
        # 如果调用位置被其他 MOD 改写，保留原版分支而不是盲目覆盖。
        self.assertIn('UNSUPPORTED_TEXT_EFFECT_SIGNATURE', self.code)
        self.assertIn('FOREIGN_TEXT_EFFECT_PATCH', self.code)
        self.assertIn('WRITE_FAILED_ROLLED_BACK', self.code)
        self.assertIn('VirtualProtect(', self.code)
        self.assertIn('FlushInstructionCache(', self.code)
        self.assertIn('DisableRandomLetterEffect(module)', self.code)

    def test_real_unpacked_dump_if_available(self):
        # 原版 PE 不随汉化分发；开发时指定路径可验证真实代码，而非只检查文本。
        location = os.environ.get('BSCN_BOND_UNPACKED')
        if not location:
            self.skipTest('尚未提供明文 EXE 路径')
        result = self.verifier.verify_dump(Path(location).read_bytes(), self.profile)
        self.assertTrue(result['text_effect_verified'])
        self.assertEqual(len(result['verified']), 5)

    def test_exact_dword_change_scenario(self):
        # 模拟分支表修改，确保其他三项的 4 字节内容完全相同。
        original = struct.pack('<4I', *[int(x,16) for x in self.effect['original_targets']])
        modified = bytearray(original)
        struct.pack_into('<I', modified, 8, int(self.effect['disabled_target_va'], 16))
        self.assertEqual(original[:8], modified[:8])
        self.assertEqual(original[12:], modified[12:])
        self.assertNotEqual(original[8:12], modified[8:12])


if __name__ == '__main__':
    unittest.main()
