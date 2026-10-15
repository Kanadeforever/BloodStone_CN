# -*- coding: utf-8 -*-
"""原生过场事件回归测试：检查 Hook 身份、队列生命周期、ABI 与诊断约束。"""
from pathlib import Path
import unittest
import struct
ROOT=Path(__file__).resolve().parents[1]
S=(ROOT/'src/native_standalone.cpp').read_text('utf-8')
Q=(ROOT/'src/subtitle_queue.h').read_text('utf-8')
ASI=ROOT.parents[1]/'release/NPCSubtitles.asi'

class CutsceneRegression(unittest.TestCase):
    def test_original_game_event_ids(self):
        # 事件名称由明文 Bond.exe 的游戏事件名称表绑定；测试不依赖外部游戏文件。
        self.assertIn('CUTSCENE_START_ID=0xA2193087u',S)
        self.assertIn('CUTSCENE_END_ID=0xA2193088u',S)
        self.assertIn('GAME_EVENT_SEND_VA=0x00E46E70u',S)
        self.assertIn('GAME_EVENT_LOCK_VA=0x004E7EE0u',S)
        self.assertIn('site+8+(I32)U32At(old+4)!=Addr(GAME_EVENT_LOCK_VA)',S)

    def test_bus_assembly_preserves_context(self):
        # 除两个过场事件外，所有消息不调用 C++，通过原指令跳板原样执行。
        part=S.split('void CutsceneBusThunk()',1)[1].split('// 硬规则',1)[0]
        for op in ('push eax','mov eax,[esp+8]','cmp eax,0xA2193087',
                   'cmp eax,0xA2193088','pushfd','pushad','popad','popfd',
                   'pop eax','jmp dword ptr [g_cutscene_bus_continue]'):
            self.assertIn(op,part)
        self.assertIn('tramp[8]=0xE9;',S)
        self.assertIn('FlushInstructionCache(GetCurrentProcess(),tramp,13)',S)
        self.assertIn('if(!Patch(site,jump,8))',S)
        self.assertIn('if(!InstallCutsceneBus())',S)
        self.assertNotIn('ORIGINAL_UI_SUBMIT_PROBE_READY',S)

    def test_clear_visible_and_waiting_without_reallocating(self):
        for piece in ('inline void Clear(Queue&q)', 'q.visible_count=0;',
                      'q.waiting_count=0;', 'q.waiting_head=0;', 'q.has_last=false;'):
            self.assertIn(piece,Q)
        self.assertIn('ClearSubtitles();',S)
        self.assertIn('subtitle_queue::Clear(g_subtitles);',S)
        self.assertIn('NATIVE_DIALOGUE_QUEUE_CLEARED',S)

    def test_no_draw_or_enqueue_during_scene(self):
        self.assertIn('if(__atomic_load_n(&g_cutscene_active,__ATOMIC_ACQUIRE))return;',S)
        self.assertIn('if(__atomic_load_n(&g_cutscene_active,__ATOMIC_ACQUIRE)){UnlockSubtitles();return;}',S)
        self.assertIn('CUTSCENE_END_QUEUE_RESUMED',S)
        self.assertIn('CUTSCENE_START_QUEUE_CLEARED',S)
        self.assertIn('if(!g_diagnostics_enabled)return;',S)
        self.assertNotIn('BATTLECHATTER_ACCEPTED_PLAYBACK_HOOK_READY',S)

    def test_binary_contains_two_event_checks(self):
        binary=ASI.read_bytes()
        self.assertEqual(binary[:2],b'MZ')
        pe=struct.unpack_from('<I',binary,0x3c)[0]
        self.assertEqual(binary[pe:pe+4],b'PE\0\0')
        self.assertEqual(struct.unpack_from('<H',binary,pe+4)[0],0x14c)
        # 二进制中同时应包含两个原生事件 ID 的小端立即数。
        for value in (0xA2193087,0xA2193088):
            self.assertIn(struct.pack('<I',value),binary)

if __name__=='__main__':unittest.main()
