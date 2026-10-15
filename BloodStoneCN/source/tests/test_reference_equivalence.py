# -*- coding: utf-8 -*-
"""可选：用开发者自己保存的旧工程做等价性复核；参考资料始终只读且不随包分发。"""
from __future__ import annotations
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'toolbox/core'))
from native_pak import NativePak, FilePak, language_blocks, read_key_entries, read_text_entries
from loc_json import import_one
from native_font import make_native_candidate

REFERENCE = os.environ.get('BSCN_PRIVATE_REFERENCE')


@unittest.skipUnless(REFERENCE, '未指定个人旧工程 BSCN_PRIVATE_REFERENCE；公开包不附带参考游戏资产')
class ReferenceEquivalenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reference = Path(REFERENCE).resolve()
        cls.archive = NativePak((cls.reference/'source/NativePakLoc/config/original/gamedata.pak').read_bytes())

    def test_runtime_and_cpp_source_remain_exactly_v12(self):
        pairs = {
            'release/Runtime/BloodstoneCN.asi': 'release/Runtime/BloodstoneCN.asi',
            'source/src/bloodstone_cn.cpp': 'source/BloodstoneCN/src/bloodstone_cn.cpp',
            'source/src/win32_api.h': 'source/BloodstoneCN/src/win32_api.h',
            'source/src/command_line.h': 'source/BloodstoneCN/src/command_line.h',
        }
        for current, reference in pairs.items():
            self.assertEqual((ROOT/current).read_bytes(), (self.reference/reference).read_bytes(), current)

    def test_readonly_file_parser_matches_original_299_resources(self):
        with FilePak(self.reference/'source/NativePakLoc/config/original/gamedata.pak') as archive:
            self.assertEqual(len(archive.entries), 299)
            for name in archive.by_name:
                self.assertEqual(archive.read_name(name), self.archive.read_name(name))

    def test_all_86_loc_match_reference_text_identity_and_non_en_bytes(self):
        total = 0
        warnings = 0
        resources = sorted(n for n in self.archive.by_name if n.endswith('.loc'))
        self.assertEqual(len(resources), 86)
        for resource in resources:
            original = self.archive.read_name(resource)
            doc = json.loads((self.reference/'release/LocTool/json'/(resource+'.json')).read_text(encoding='utf-8'))
            result, detail = import_one(original, doc, resource)
            frozen = (self.reference/'release/Runtime/BSCNContents/loc'/resource.removeprefix('gamedata/xt/loc/')).read_bytes()
            self.assertEqual(read_key_entries(result), read_key_entries(frozen))
            for lang in ('en', 'fr', 'de', 'it', 'ja', 'pl', 'ru', 'es'):
                self.assertEqual(read_text_entries(result, lang), read_text_entries(frozen, lang), (resource, lang))
            for (_, a, n), (_, b, m) in zip(language_blocks(frozen)[1:], language_blocks(result)[1:]):
                self.assertEqual(frozen[a:a+n], result[b:b+m])
            total += len(doc['entries'])
            warnings += len(detail)
        self.assertEqual(total, 7975)
        self.assertEqual(warnings, 7)

    def test_same_atlas_generates_byte_identical_2048_fonts(self):
        folder = self.reference/'source/NativeEurFont/config/original'
        dat, bpr, _report = make_native_candidate((folder/'ui_font_eur.dat').read_bytes(),
                                                 (folder/'ui_font_eur.bpr').read_bytes(),
                                                 (folder/'fonts.bfa').read_bytes())
        runtime = self.reference/'release/Runtime/BSCNContents/fonts'
        self.assertEqual(dat, (runtime/'ui_font_cnx.dat').read_bytes())
        self.assertEqual(bpr, (runtime/'ui_font_cnx.bpr').read_bytes())

    def test_real_private_inputs_complete_new_prepare_build_verify_workflow(self):
        # 优先使用明确提供的真实根包；只有独立旧工程时使用手动模板入口。
        from test_public_toolkit import copy_public_tools
        with tempfile.TemporaryDirectory(dir=ROOT/'.build') as tmp:
            tools = Path(tmp)/'public tools 中文'
            copy_public_tools(tools)
            source = self.reference/'source/NativePakLoc/config/original/gamedata.pak'
            template = self.reference/'source/NativeEurFont/config/original'
            environment = dict(os.environ, PYTHONUTF8='1')
            def run(*args):
                proc = subprocess.run([sys.executable, '-B', str(tools/'toolkit.py'), *args],
                                      capture_output=True, text=True, encoding='utf-8', env=environment)
                self.assertEqual(proc.returncode, 0, proc.stdout+proc.stderr)
            if os.environ.get('BSCN_PAK_REFERENCE'):
                run('prepare', '--gamedata', str(source), '--root-pak',
                    str(Path(os.environ['BSCN_PAK_REFERENCE'])/'root.pak'))
            else:
                run('prepare', '--gamedata', str(source), '--template-dir', str(template))
            shutil.rmtree(tools/'translations')
            shutil.copytree(self.reference/'release/LocTool/json', tools/'translations')
            shutil.copyfile(template/'fonts.bfa', tools/'input/template/fonts.bfa')
            run('build')
            run('verify')
            report = json.loads((tools/'output/build_manifest.json').read_text(encoding='utf-8'))
            self.assertEqual(report['verification']['loc_files'], 86)
            self.assertEqual(report['verification']['entries'], 7975)
            self.assertEqual(report['verification']['texture_size'], 2048)
            for resource in (n for n in self.archive.by_name if n.endswith('.loc')):
                actual = (tools/'output/Runtime/BSCNContents/loc'/resource.removeprefix('gamedata/xt/loc/')).read_bytes()
                frozen = (self.reference/'release/Runtime/BSCNContents/loc'/resource.removeprefix('gamedata/xt/loc/')).read_bytes()
                self.assertEqual(read_text_entries(actual, 'en'), read_text_entries(frozen, 'en'))


if __name__ == '__main__':
    unittest.main()
