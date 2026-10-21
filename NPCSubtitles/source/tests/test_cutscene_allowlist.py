# -*- coding: utf-8 -*-
"""内置四条过场白名单及六条已移除规则的源代码与原生行为验证。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import struct
ROOT=Path(__file__).resolve().parents[1]
SRC=(ROOT/'src/native_standalone.cpp').read_text('utf-8')
HEADER=(ROOT/'src/cutscene_allowlist.h').read_text('utf-8')
ASI=ROOT.parents[1]/'release/NPCSubtitles.asi'

class CutsceneAllowlistTest(unittest.TestCase):
    def test_exact_four_entries_and_six_removed_entries(self):
        kept={'9CF9A69B':'Bangkok_Escape_01_RG01_77',
              '8350582A':'Bangkok_Escape_01_RG02_116',
              '4C746585':'Istanbul_0_01_BG01_394',
              'D6C1A0AB':'Refinery_1_01_BOND_192'}
        removed={'E3146C1A':'Dam_Works_2_01_BOND_CS_1',
                 '8B43BA09':'Dam_Works_2_01_BOND_CS_3',
                 '74028F57':'Dam_Works_2_01_BOND_CS_4',
                 'CCE93E4F':'Monaco_Driving_01_NICOLE_CS_68',
                 '0713CDE3':'Dam_Works_2_01_RAK_11',
                 '34584E74':'Dam_Works_2_01_RAK_35'}
        self.assertEqual(HEADER.count('u,"'),4)
        image=ASI.read_bytes()
        for val,key in kept.items():
            self.assertIn('0x'+val+'u',HEADER)
            self.assertIn(key,HEADER)
            self.assertIn(struct.pack('<I',int(val,16)),image)
        for val,key in removed.items():
            self.assertNotIn('0x'+val+'u',HEADER)
            self.assertNotIn(key,HEADER)
        self.assertNotIn('0xCAC03B4Fu',HEADER)
        self.assertIn('CUTSCENE_ALLOWLIST_EMBEDDED',SRC)


    def test_removed_hashes_absent_from_active_binary(self):
        # 编译后再次检查机器码和常量区，避免只改了源码头文件却交付旧 ASI。
        image=ASI.read_bytes()
        for val in ('E3146C1A','8B43BA09','74028F57','CCE93E4F','CAC03B4F','0713CDE3','34584E74'):
            self.assertNotIn(struct.pack('<I',int(val,16)),image,val)

    def test_allowlist_bypasses_only_allowed_gates(self):
        for line in ('IsAllowlistedEntry(hash,key)', 'const bool allowlisted=IsAllowlisted(hash)',
                     'if(!npc_event&&!allowlisted)', 'if(inside_scene&&!allowlisted)',
                     'if(!allowlisted&&__atomic_load_n(&g_cutscene_active',
                     'subtitle_queue::Retain(g_subtitles,IsAllowlisted)',
                     'CUTSCENE_ALLOWLIST_QUEUED_IN_SCENE',
                     'if(IsAllowlisted(visible[i].hash))'):
            self.assertIn(line,SRC)
        self.assertIn('CUTSCENE_ALLOWLIST_EMBEDDED',SRC)
        self.assertNotIn('BATTLECHATTER_ACCEPTED_PLAYBACK_HOOK_READY',SRC)
        self.assertNotIn('ORIGINAL_UI_SUBMIT_PROBE_READY',SRC)

    def test_real_native_queue_behavior(self):
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler:self.skipTest('当前环境没有用于原生单元测试的 C++ 编译器')
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/'queue_test'
            subprocess.run([compiler,'-std=c++17','-O2',
                            str(ROOT/'tests/allowlist_queue_native_test.cpp'),
                            '-o',str(target)],check=True,capture_output=True,text=True)
            subprocess.run([str(target)],check=True,timeout=15,capture_output=True,text=True)

if __name__=='__main__': unittest.main()
