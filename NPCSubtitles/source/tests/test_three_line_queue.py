# -*- coding: utf-8 -*-
"""对白队列专项验证：检验真正被 ASI 包含的调度头文件，而不模拟另一套逻辑。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'src/native_standalone.cpp').read_text('utf-8')
HEADER=(ROOT/'src/subtitle_queue.h').read_text('utf-8')

class ThreeLineQueue(unittest.TestCase):
    def test_runtime_is_not_single_slot(self):
        """不再允许一个全局字符串被每条新对白直接覆盖。"""
        self.assertIn('subtitle_queue::Queue g_subtitles={};',SOURCE)
        self.assertIn('subtitle_queue::Push(g_subtitles,hash,entry->text,now,duration)',SOURCE)
        self.assertIn('subtitle_queue::Advance(g_subtitles,GetTickCount())',SOURCE)
        self.assertNotIn('wchar_t g_text[MAX_TEXT]',SOURCE)
        self.assertNotIn('U32 g_last_hash=0,g_start=0,g_end=0',SOURCE)

    def test_preserves_native_route_and_single_d3d_draw(self):
        """变更局限于队列/三行布局，不能改动已实机通过的字幕分流和图形 Hook。"""
        self.assertIn('ROUTE_FLAG_OFFSET=0x1Cu',SOURCE)
        self.assertIn('NPC_NATIVE_EVENT_SUPPRESSED',SOURCE)
        self.assertIn('MAX_VERTICES=3u*MAX_TEXT*6u',SOURCE)
        self.assertIn('subtitle_queue::AllocateLineBudget(active,g_requested_lines,budget)',SOURCE)
        self.assertIn('const float max_width=float(vp.width)*0.76f;',SOURCE)
        self.assertIn('render(device,19,2);render(device,20,4);',SOURCE)
        self.assertIn('Draw(device);',SOURCE)
        self.assertIn('const auto enqueue_result=subtitle_queue::Push(',SOURCE)
        self.assertNotIn('BATTLECHATTER_',SOURCE)

    def test_fixed_bounded_fifo_and_timestamps(self):
        """同时三条、16 条等待、独立期限以及拥挤保护必须进入实际运行代码。"""
        for fragment in ('kVisible = 3;', 'kWaiting = 16;',
                         'item.started_ms=now;', 'q.waiting_count--;',
                         'return PushResult::Full;', 'kDuplicateMs = 800;',
                         'inline void AllocateLineBudget'):
            self.assertIn(fragment,HEADER)
        self.assertIn('NPC_SUBTITLE_QUEUE_FULL',SOURCE)
        self.assertIn('NPC_SUBTITLE_DEFERRED',SOURCE)
        self.assertIn('LockSubtitles();',SOURCE)
        self.assertIn('UnlockSubtitles();',SOURCE)

    def test_compile_and_run_real_queue(self):
        """可用编译器时实际执行 C++ 队列验证；缺少编译器时不冒称已运行。"""
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('当前主机没有 C++ 原生测试编译器；交叉编译仍可独立进行')
        with tempfile.TemporaryDirectory(prefix='npc_queue_test_') as directory:
            result=Path(directory)/('queue_test.exe' if __import__('os').name=='nt' else 'queue_test')
            source=ROOT/'tests/queue_native_test.cpp'
            subprocess.run([compiler,'-std=c++17','-O2',str(source),'-o',str(result)],check=True,timeout=30,capture_output=True)
            subprocess.run([str(result)],check=True,timeout=15,capture_output=True)

if __name__=='__main__':
    unittest.main()
