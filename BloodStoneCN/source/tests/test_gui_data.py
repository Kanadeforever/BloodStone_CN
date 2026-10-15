# -*- coding: utf-8 -*-
"""验证 GUI 的数据保存和后台任务，不依赖图形桌面或任何游戏资产。"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'toolbox/core'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor import TranslationDocument
from jobs import JobRunner
from synthetic_fixtures import make_game
from test_public_toolkit import copy_public_tools


class GuiDataTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT/'.build')
        self.base = Path(self.temporary.name)
        self.tools = self.base/'独立 工具箱'
        copy_public_tools(self.tools)
        game = self.base/'游戏'
        make_game(game)
        process = subprocess.run([sys.executable, '-B', str(self.tools/'toolkit.py'),
                                  'prepare', '--game', str(game)],
                                 env=dict(os.environ, PYTHONUTF8='1'), capture_output=True,
                                 text=True, encoding='utf-8')
        self.assertEqual(process.returncode, 0, process.stdout+process.stderr)
        self.relative = 'gamedata/xt/loc/common.loc.json'
        self.path = self.tools/'translations'/self.relative

    def tearDown(self):
        self.temporary.cleanup()

    def test_unicode_multiline_edit_preserves_all_identity_fields(self):
        original = json.loads(self.path.read_text(encoding='utf-8'))
        model = TranslationDocument(self.tools, self.relative)
        model.update(0, '中文第一行\n第二行')
        model.save()
        current = json.loads(self.path.read_text(encoding='utf-8'))
        expected = json.loads(json.dumps(original))
        expected['entries'][0]['translate'] = '中文第一行\n第二行'
        self.assertEqual(current, expected)
        self.assertEqual(model.matching_indices('中文第一行'), [0])
        self.assertFalse(model.dirty)
        self.assertNotIn(b'\n', self.path.read_bytes().replace(b'\r\n', b''))

    def test_invalid_translation_does_not_replace_saved_json(self):
        model = TranslationDocument(self.tools, self.relative)
        before = self.path.read_bytes()
        model.update(0, '非法\0文本')
        with self.assertRaisesRegex(ValueError, 'NUL'):
            model.save()
        self.assertEqual(self.path.read_bytes(), before)
        self.assertTrue(model.dirty)

    def test_external_editor_changes_are_preserved(self):
        model = TranslationDocument(self.tools, self.relative)
        external = json.loads(self.path.read_text(encoding='utf-8'))
        external['entries'][0]['translate'] = '外部编辑器修改'
        self.path.write_text(json.dumps(external, ensure_ascii=False), encoding='utf-8')
        before = self.path.read_bytes()
        model.update(0, 'GUI 修改')
        with self.assertRaisesRegex(ValueError, '其他程序修改'):
            model.save()
        self.assertEqual(self.path.read_bytes(), before)

    def test_editor_rejects_changed_resource_identity(self):
        doc = json.loads(self.path.read_text(encoding='utf-8'))
        doc['entries'][0]['original'] = '被破坏的原文身份'
        self.path.write_text(json.dumps(doc, ensure_ascii=False), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'original'):
            TranslationDocument(self.tools, self.relative)

    def test_background_runner_returns_logs_and_failure_without_blocking_ui(self):
        runner = JobRunner(self.tools)
        runner.start(['status'])
        lines = []
        while True:
            kind, value = runner.events.get(timeout=15)
            if kind == 'done':
                self.assertEqual(value, 0)
                break
            lines.append(value)
        state = json.loads(next(line.removeprefix('[状态] ') for line in lines if line.startswith('[状态] ')))
        self.assertTrue(state['initialized'])
        self.assertEqual(state['translation_files'], 86)
        runner.start(['configure-font', '--pixel-size', '99'])
        failed = []
        while True:
            kind, value = runner.events.get(timeout=15)
            if kind == 'done':
                self.assertNotEqual(value, 0)
                break
            failed.append(value)
        self.assertTrue(any('16～34' in line for line in failed))
        self.assertFalse(any('Traceback' in line for line in failed))


if __name__ == '__main__':
    unittest.main()
