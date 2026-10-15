# -*- coding: utf-8 -*-
"""检查诊断模式是否真正受命令行控制，以及日志是否足以支持用户报错。

本测试读取实际参与 Win32 编译的源码，避免只修改文档而未改变 DLL 行为。
Windows 实机是否正确加载游戏仍需用户验证，不能由这些静态合同测试代替。
"""
import re
import unittest
from pathlib import Path

BASE = Path(__file__).resolve().parents[1]
CPP = (BASE / 'src/native_standalone.cpp').read_text(encoding='utf-8')
HEADER = (BASE / 'src/win32_minimal.h').read_text(encoding='utf-8')
IMPORTS = (BASE / 'tools/kernel32.def').read_text(encoding='utf-8')


class CommandLineLogging(unittest.TestCase):
    def test_exact_switch_parser_is_called_before_startup_file(self):
        """入口必须先解析完整独立参数，不能用命令行子串模糊判断。"""
        start = CPP.index('extern "C" BOOL WINAPI DllMain(')
        entry = CPP[start:]
        self.assertIn('GetCommandLineW()', CPP)
        self.assertIn('bool CommandLineHasLogFlag()', CPP)
        self.assertIn('if(argument>0&&length==5', CPP)
        self.assertIn('if(!quoted&&(', CPP)
        self.assertIn('while(*p==L\'\\\\\')', CPP)
        self.assertLess(entry.index('g_diagnostics_enabled=CommandLineHasLogFlag()'),
                        entry.index('Checkpoint(L"NPCSubtitles_boot.log"'))

    def test_all_three_logs_gated_without_mutating_old_files(self):
        """无 --log 时普通日志及启动标记都直接返回，不创建或追加文件。"""
        logger = CPP[CPP.index('void Log(const char* key'): CPP.index('void LogPair(')]
        checkpoint = CPP[CPP.index('void Checkpoint('): CPP.index('U32 WINAPI Worker(', CPP.index('void Checkpoint('))]
        worker = CPP[CPP.index('U32 WINAPI Worker('): CPP.index('extern "C" BOOL WINAPI DllMain(')]
        self.assertLess(logger.index('if(!g_diagnostics_enabled)return;'),logger.index('OutputDebugStringA(line)'))
        self.assertLess(checkpoint.index('if(!g_diagnostics_enabled)return;'),checkpoint.index('CreateFileW('))
        self.assertIn('if(g_diagnostics_enabled){\n  g_log=CreateFileW',worker)
        for filename in ('NPCSubtitles.log', 'NPCSubtitles_boot.log', 'NPCSubtitles_phase.log'):
            self.assertIn(filename,CPP)
        self.assertNotIn('DeleteFileW(',CPP)

    def test_detailed_actionable_logging(self):
        """故障报告必须包含版本/时间/线程、文件与资源、Hook、文字与渲染情况。"""
        for field in ('GetLocalTime(&now)', 'GetCurrentThreadId()', 'GetTickCount()',
                      'LogPath("ASI_MODULE_DIRECTORY"', 'LOG_MODE=ENABLED',
                      'LOC_FILE_SCANNED', 'LOC_FILE_SKIPPED',
                      'NPC_DIALOGUE_INDEX_MISS', 'NPC_DIALOGUE_FILTERED_HASH',
                      'NPC_DIALOGUE_AMBIGUOUS_HASH','LogHash("NPC_SUBTITLE_QUEUE_HASH",hash,entry->text)',
                      'D3D9_VIEWPORT_CHANGED', 'D3D9_DEVICE_RESET_FAILED',
                      'NPC_DRAW_FAILED','FONT_TEXTURE_SIZE', 'NATIVE_FONT_CAPTURE_WAIT',
                      'STATS render_frames/resets', 'STATS hits/miss',
                      'LogFailure(', 'NATIVE_FONT_RESOURCE_STATUS'):
            self.assertIn(field,CPP,field)

    def test_high_frequency_output_is_limited(self):
        """EndScene 高频循环必须限时聚合，而不是逐帧输出大量日志。"""
        self.assertIn('now-g_diag_last_report<30000',CPP)
        self.assertIn('count<=512||count%128==0',CPP)
        endscene=CPP[CPP.index('extern "C" HRESULT WINAPI EndSceneHook('): CPP.index('extern "C" HRESULT WINAPI ResetHook(')]
        self.assertIn('if(g_diagnostics_enabled)ReportStats()',endscene)
        self.assertNotIn('Log("FRAME_',endscene)

    def test_system_imports_match_sdk_free_abi(self):
        """时间、线程与命令行 API 必须进入独立 Kernel32 导入表。"""
        for name in ('GetCommandLineW','GetCurrentThreadId','GetLocalTime','GetLastError'):
            self.assertIn(name,HEADER)
            self.assertRegex(IMPORTS,rf'(?m)^ {name}$')
        self.assertIn('sizeof(SystemTime)==16',HEADER)

    def test_battlechatter_experiment_not_reintroduced(self):
        """只改变稳定的 NPC 版，不恢复未经实机验证的战斗喊话 Hook。"""
        self.assertNotIn('BATTLECHATTER_ACCEPTED_PLAYBACK_HOOK_READY',CPP)
        self.assertNotIn('0x00B42938',CPP)
        self.assertIn('float AutoScale(U32 height)',CPP)
        self.assertIn('NPC_NATIVE_EVENT_SUPPRESSED',CPP)

if __name__ == '__main__':
    unittest.main()
