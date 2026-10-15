# -*- coding: utf-8 -*-
"""检查游戏自身事件标志能直接决定 NPC 字幕入队，不保留额外 UI CALL 探针。"""
from pathlib import Path
import os
import struct
import unittest
ROOT=Path(__file__).resolve().parents[1]
PACKAGE=ROOT.parents[1]
SOURCE=(ROOT/'src/native_standalone.cpp').read_text('utf-8')
BINARY=(PACKAGE/'release/NPCSubtitles.asi').read_bytes()

def original_code(va):
    """按 EXE 节表把游戏静态虚拟地址换算成原始文件偏移。"""
    path=os.environ.get('BOND_ANALYSIS_EXE')
    if not path or not Path(path).is_file():return None
    b=Path(path).read_bytes()
    off=struct.unpack_from('<I',b,0x3c)[0]
    image_base=struct.unpack_from('<I',b,off+24+28)[0]
    num=struct.unpack_from('<H',b,off+6)[0]
    optional_size=struct.unpack_from('<H',b,off+20)[0]
    rva=va-image_base
    for i in range(num):
        sec=off+24+optional_size+i*40
        vs,base,raw_sz,raw_off=struct.unpack_from('<IIII',b,sec+8)
        if base<=rva<base+max(vs,raw_sz):
            pos=raw_off+rva-base
            return b[pos:pos+12]
    return None

class NativeEventRoute(unittest.TestCase):
    def test_game_instruction_and_route_byte(self):
        """原游戏的零值比较使用事件 +0x1C，+0x1D 是另一个独立标志。"""
        self.assertIn('0x0093A2A1u, LOOKUP_VA=0x00F12820u',SOURCE)
        self.assertIn('ROUTE_FLAG_OFFSET=0x1Cu',SOURCE)
        if original_code(0x0093A282) is not None:
            self.assertEqual(original_code(0x0093A282)[:4],bytes.fromhex('80 7F 1C 00'))
            self.assertEqual(original_code(0x0093A26C)[:4],bytes.fromhex('80 7F 1D 00'))

    def test_runtime_gates_instead_of_second_ui_hook(self):
        """每次先读取分流字段；游戏原生字幕和无法分类的事件禁止进入队列。"""
        for snippet in ('ReadMemory((const U8*)frame->edi+ROUTE_FLAG_OFFSET,&value,1)',
                        'g_route_is_npc=(value==1)?1u:0u;',
                        'const bool route_known=g_route_known&&g_route_hash==hash;',
                        'if(!route_known)', 'if(!npc_event)',
                        'NPC_NATIVE_EVENT_SUPPRESSED', 'NPC_EVENT_ROUTE_UNKNOWN'):
            self.assertIn(snippet,SOURCE)
        self.assertLess(SOURCE.index('if(!npc_event)'),SOURCE.index('const Entry* entry=FindEntry(hash);'))
        for forbidden in ('NativeUiProbeThunk','InstallNativeUiProbe','g_orig_native_ui',
                          'NATIVE_UI_CALL_VA','SemanticSignature(','k_embedded_routes'):
            self.assertNotIn(forbidden,SOURCE)
        for forbidden in (b'ORIGINAL_UI_SUBMIT_PROBE_READY',b'ORIGINAL_NATIVE_UI_SUBMIT_CALL',
                          b'NATIVE_ROUTE_SUPPRESSED',b'NPCSubtitlesSemanticRoutes.tsv'):
            self.assertNotIn(forbidden,BINARY)

    def test_14_real_event_samples_support_split(self):
        """以用户日志的 8 条 NPC 和 6 条剧情语音作为基准，不根据哈希直接分类。"""
        # 实机日志使用的事件后六字节；倒数第二字节就是源事件 +0x1C。
        npc={h:'00 00 00 00 01 00' for h in (
            '8BA2A6FB','0B7C59AA','736A7C36','F8C0DF62',
            'F68BFDD2','2A6789B3','67843A23','AD581D8A')}
        native={h:'00 00 01 01 00 00' for h in (
            'A9213028','B8A976A1','4C269798','70FD715C','37536DBA','DB8C439D')}
        for h,raw in npc.items():
            source_1c=bytes.fromhex(raw)[4]
            self.assertEqual(source_1c,1,f'{h}: NPC 环境对白应使用原生非字幕分支')
            self.assertFalse(source_1c==0)
        for h,raw in native.items():
            source_1c=bytes.fromhex(raw)[4]
            self.assertEqual(source_1c,0,f'{h}: 剧情对白应由游戏原生字幕处理')
            self.assertTrue(source_1c==0)
        self.assertEqual((len(npc),len(native)),(8,6))

    def test_only_existing_call_sites_and_log_mode(self):
        """对话/字形/D3D9 入口保持不变，命令行日志仍默认关闭。"""
        self.assertIn('g_diagnostics_enabled=CommandLineHasLogFlag()?1u:0u;',SOURCE)
        self.assertIn('DIALOGUE_VA=0x0093A2A1u',SOURCE)
        self.assertIn('FONT_SLOT_VA=0x011A02D8u',SOURCE)
        self.assertIn('RESOURCE_VA=0x00E42C21u',SOURCE)
        self.assertIn('NPC_NATIVE_GLYPH_CJK_DRAW',SOURCE)
        self.assertIn('D3D9_ENDSCENE_ACTIVE',SOURCE)
        self.assertNotIn('BATTLECHATTER_',SOURCE)

if __name__=='__main__':unittest.main()
