# -*- coding: utf-8 -*-
"""原生目录索引的结构回归，以及可选的用户双 PAK 只读验证。"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'toolbox/core'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_pak import NativePak, FilePak, align_up, read_u32
from synthetic_fixtures import make_tree_pak, make_game
from test_public_toolkit import copy_public_tools

PAK_REFERENCE = os.environ.get('BSCN_PAK_REFERENCE')
EXPECTED = {
    'gamedata.pak': '070b7592885c36e2b788b43a0b9ed8f28cf36b8b087a734e4c6e700ed2629fb4',
    'root.pak': 'ff8a5ba04a336e05fae2cdc52720373742adc62cc9c4b153f4167d486552df10',
    'ui_font_eur.dat': 'a12fefdfc15d1fbd55d19739d9031faf637f837d37b2febba4a1f2fc8d50b7bb',
    'ui_font_eur.bpr': 'd4e46ebcaa4b82e75dc68fb5c8fc725c03df5d77f405eef0ddcc906f8768509f',
}


def digest(path: Path) -> str:
    """只读流式指纹，不把整个大型根包额外复制进 Python 堆。"""
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


class DirectoryIndexTests(unittest.TestCase):
    def test_parent_paths_disambiguate_same_leaf_and_zero_last_field(self):
        resources = {'a/common.bin': b'first', 'b/common.bin': b'second',
                     'ui/data/font.dat': b'font data'}
        archive = NativePak(make_tree_pak(resources))
        self.assertEqual(archive.version, 3)
        self.assertEqual(set(archive.by_name), set(resources))
        for name, payload in resources.items():
            self.assertEqual(archive.read_name(name), payload)

    def test_directory_cycle_is_rejected(self):
        data = bytearray(make_tree_pak({'ui/data/font.dat': b'data'}))
        entry = NativePak(data).by_name['ui/data/font.dat']
        alignment = read_u32(data, 8)
        first = align_up(alignment + 4 + read_u32(data, alignment), alignment) + 12
        parent = read_u32(data, entry.record_offset + 4)
        struct.pack_into('<I', data, first + parent + 4, parent)
        with self.assertRaisesRegex(ValueError, '循环'):
            NativePak(data)

    def test_bad_parent_offset_and_file_parent_are_rejected(self):
        original = make_tree_pak({'ui/data/font.dat': b'data'})
        entry = NativePak(original).by_name['ui/data/font.dat']
        alignment = read_u32(original, 8)
        first = align_up(alignment + 4 + read_u32(original, alignment), alignment) + 12
        for offset, message in [(1, '未对齐'), (0xFFFFFFFE, '越界'),
                                (entry.record_offset - first, '普通文件')]:
            with self.subTest(offset=offset):
                data = bytearray(original)
                struct.pack_into('<I', data, entry.record_offset + 4, offset)
                with self.assertRaisesRegex(ValueError, message):
                    NativePak(data)

    def test_component_traversal_is_rejected(self):
        with self.assertRaisesRegex(ValueError, '不安全'):
            NativePak(make_tree_pak({'ui/../escape.dat': b'data'}))

    def test_game_child_toolbox_can_initialize(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'.build') as tmp:
            game = Path(tmp)/'游戏'
            make_game(game)
            tools = game/'汉化工程/toolbox'
            copy_public_tools(tools)
            process = subprocess.run([sys.executable, '-B', str(tools/'toolkit.py'),
                                      'prepare', '--game', str(game)],
                                     capture_output=True, text=True, encoding='utf-8',
                                     env=dict(os.environ, PYTHONUTF8='1'))
            self.assertEqual(process.returncode, 0, process.stdout+process.stderr)
            self.assertTrue((tools/'work/project.json').is_file())


@unittest.skipUnless(PAK_REFERENCE, '未指定个人双 PAK 目录 BSCN_PAK_REFERENCE；公开包不附带游戏资源')
class ActualPakTests(unittest.TestCase):
    def test_exact_user_paks_and_extracted_eur_match_known_fingerprints(self):
        cache = Path(PAK_REFERENCE)
        for name in ('gamedata.pak', 'root.pak'):
            self.assertEqual(digest(cache/name), EXPECTED[name])
        with FilePak(cache/'root.pak') as archive:
            self.assertEqual(archive.version, 3)
            self.assertEqual(len(archive.entries), 10409)
            self.assertEqual(archive.empty_entries, 525)
            for name in ('ui_font_eur.dat', 'ui_font_eur.bpr'):
                raw = archive.read_name('ui/data/'+name)
                self.assertEqual(hashlib.sha256(raw).hexdigest(), EXPECTED[name])

    def test_actual_auto_prepare_and_loc_build_in_game_child(self):
        cache = Path(PAK_REFERENCE)
        with tempfile.TemporaryDirectory(dir=ROOT/'.build') as tmp:
            game = Path(tmp)/'游戏'
            (game/'cache').mkdir(parents=True)
            for name in ('gamedata.pak', 'root.pak'):
                shutil.copyfile(cache/name, game/'cache'/name)
            tools = game/'汉化工程/toolbox'
            copy_public_tools(tools)
            for arguments in [('prepare', '--game', str(game)), ('build-loc',)]:
                result = subprocess.run([sys.executable, '-B', str(tools/'toolkit.py'), *arguments],
                                        capture_output=True, text=True, encoding='utf-8',
                                        env=dict(os.environ, PYTHONUTF8='1'))
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            report = json.loads((tools/'work/project.json').read_text(encoding='utf-8'))
            self.assertEqual(report['pak_versions'], {'gamedata': 2, 'root': 3})
            self.assertEqual(report['source_root_sha256'], EXPECTED['root.pak'])
            self.assertTrue(report['matches_reference_assets'])
            self.assertEqual(report['entries'], 7975)
            self.assertEqual(len(list((tools/'output/loc/BSCNContents/loc').rglob('*.loc'))), 86)
            for name in ('gamedata.pak', 'root.pak'):
                self.assertEqual(digest(game/'cache'/name), EXPECTED[name])


if __name__ == '__main__':
    unittest.main()
