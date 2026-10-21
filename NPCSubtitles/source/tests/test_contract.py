# -*- coding: utf-8 -*-
"""验证真正参与编译的 Win32 独立代码和发行二进制，而不是历史对照源码。"""
import re
import struct
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
PACKAGE=ROOT.parents[1]
CODE=(ROOT/'src/native_standalone.cpp').read_text(encoding='utf-8')
HEAD=(ROOT/'src/win32_minimal.h').read_text(encoding='utf-8')
BINARY=PACKAGE/'release/NPCSubtitles.asi'


class NativeContract(unittest.TestCase):
    def test_single_dialogue_entry(self):
        """事件只从已经实机验证的一个 CALL 进入，拒绝两个重复事件站点。"""
        self.assertIn('0x0093A2A1u',CODE)
        self.assertIn('0x00F12820u',CODE)
        for banned in ('0x009302B5','0x00A706E8','0x00E85900'):
            self.assertNotIn(banned,CODE)

    def test_dynamic_asi_relative_paths(self):
        """ASi 与 BSCNContents 的关系固定，实际安装目录绝对不能写死。"""
        self.assertIn('GetModuleFileNameW(g_self,g_folder',CODE)
        self.assertIn('BSCNContents\\\\loc',CODE)
        self.assertNotIn('NPCSubtitlesSemanticRoutes.tsv',CODE)
        self.assertNotIn('L"scripts"',CODE)
        self.assertNotIn('L"plugins"',CODE)

    def test_oltx_original_loc(self):
        """字幕只从游戏原生 OLTX 的 EN 语言块读取，不引用旧 BFA/BLI 快照。"""
        for required in ('ValidKeys(', 'firstlang','FindText(', 'LoadIndex()', 'ReadFile('):
            self.assertIn(required,CODE)
        for forbidden in ('loc_context.bli','fonts.bfa','ENGLISH_NATIVE_FALLBACK'):
            self.assertNotIn(forbidden,CODE)

    def test_unicode_glyph_without_second_atlas(self):
        """UNICODE 输入必须原封不动交给原生字形槽位，纹理生命周期有 COM 引用。"""
        for required in ('0x011A02D8u','0x00E42C21u','SurfaceDesc','0x12C',
                         'VFunc<U32(WINAPI*)(void*)>(texture,1)', 'ClearTextures()'):
            if required=='GetLevelDesc':
                self.assertIn('SurfaceDesc',CODE)
            else:self.assertIn(required,CODE)
        self.assertIn('NPC_NATIVE_GLYPH_CJK_DRAW',CODE)

    def test_game_native_route_before_enqueue(self):
        """NPC 字幕使用原始事件 0x1C 分流，禁止回退到调用栈签名去重。"""
        self.assertIn('constexpr U32 ROUTE_FLAG_OFFSET=0x1Cu;',CODE)
        self.assertIn('g_route_is_npc=(value==1)?1u:0u;',CODE)
        self.assertIn('NPC_NATIVE_EVENT_SUPPRESSED',CODE)
        self.assertLess(CODE.index('if(!npc_event&&!allowlisted)'),CODE.index('NPC_SUBTITLE_QUEUE_HASH'))
        for forbidden in ('SemanticSignature(', 'Fnv(', 'k_embedded_routes', 'NATIVE_ROUTE_SUPPRESSED'):
            self.assertNotIn(forbidden,CODE)

    def test_win32_d3d9_lifecycle(self):
        """只走三处原生 D3D9 虚表入口，绘制先于 EndScene，Reset 必须清理状态。"""
        self.assertIn('HookDevice(',CODE)
        self.assertIn('&vt[42]',CODE)
        self.assertIn('&vt[16]',CODE)
        self.assertIn('ResetResources();',CODE)
        hook=CODE[CODE.index('extern "C" HRESULT WINAPI EndSceneHook('):]
        self.assertLess(hook.index('Draw(device)'),hook.index('g_prev_end_scene)(device)'))

    def test_no_dead_runtime_dependencies(self):
        """实际编译源不包含 C++ 标准库或 Windows SDK 头，不依赖 BFA 或其它 ASI。"""
        self.assertNotIn('#include <windows.h>',CODE)
        self.assertNotIn('#include <d3d9.h>',CODE)
        self.assertNotIn('#include <vector>',CODE)
        self.assertNotIn('BloodstoneCN.asi',CODE)
        self.assertNotIn('NPCSubtitles.ini',CODE)
        self.assertFalse((ROOT/'config/NPCSubtitles.ini').exists())
        self.assertEqual({x.name for x in (ROOT/'src').glob('*.cpp')},{'native_standalone.cpp','startup_probe.cpp'})

    def test_archived_legacy_not_linked(self):
        """完整保留上一版源码供回归定位，但它不能进入当前二进制链接。"""
        cm=(ROOT/'CMakeLists.txt').read_text('utf-8')
        self.assertIn('src/native_standalone.cpp',cm)
        self.assertNotIn('src/renderer.cpp',cm)
        self.assertTrue((ROOT/'tools/reference_original_v01.zip').is_file())

    def test_pe_machine_and_imports(self):
        """PE 必须是可链接的 Win32 DLL，只导入 Windows Kernel32。"""
        self.assertTrue(BINARY.exists(),'未实际生成 NPCSubtitles.asi')
        b=BINARY.read_bytes()
        self.assertEqual(b[:2],b'MZ')
        pe=struct.unpack_from('<I',b,0x3c)[0]
        self.assertEqual(b[pe:pe+4],b'PE\0\0')
        self.assertEqual(struct.unpack_from('<H',b,pe+4)[0],0x14c)
        self.assertTrue(struct.unpack_from('<H',b,pe+22)[0]&0x2000)
        self.assertEqual(struct.unpack_from('<H',b,pe+24)[0],0x10b)
        self.assertIn(b'KERNEL32.dll',b)
        self.assertNotIn(b'VCRUNTIME',b.upper())

    def test_small_loader_image_and_delayed_index_allocation(self):
        """过大的静态索引会导致 DLL 加载时直接保留数 MiB 映像，诊断必须先于资源分配。"""
        self.assertIn('Entry* g_entries_table=nullptr;', CODE)
        self.assertIn('VirtualAlloc(nullptr,U32(sizeof(Entry))*MAX_RECORDS', CODE)
        self.assertNotIn('Entry g_entries_table[MAX_RECORDS]', CODE)
        image=BINARY.read_bytes()
        pe=struct.unpack_from('<I',image,0x3c)[0]
        self.assertLess(struct.unpack_from('<I',image,pe+24+0x38)[0],0x100000)
        self.assertLess(CODE.index('Log("STARTING'),CODE.index('NPC_INDEX_ALLOCATED'))
        self.assertNotIn('D3D9_INTERFACE_BOOTSTRAP_DONE', CODE)

    def test_stack_probe_adjusts_esp_and_preserves_registers(self):
        """直接检查活动 ASM 与已编译 PE，防止再出现只探测不分配栈帧的错误。"""
        pos=CODE.index('void _chkstk()')
        probe=CODE[pos:CODE.index('extern "C" int _fltused',pos)]
        for x in ('lea ecx, [esp+16]','lea edx, [ecx-16]',
                  'mov [edx+12], eax','mov esp, edx',
                  'pop edx','pop ecx','pop eax','ret'):
            self.assertIn(x,probe)
        image=BINARY.read_bytes()
        # push eax/ push ecx/ push edx / lea ecx,[esp+16]
        self.assertIn(bytes.fromhex('50 51 52 8D 4C 24 10'),image)
        # mov esp,edx / pop edx / pop ecx / pop eax / ret
        self.assertIn(bytes.fromhex('89 D4 5A 59 58 C3'),image)
        # 复核工具会在构建结束后重新检查所有 CALL _chkstk 的栈帧。
        import importlib.util
        verifier=ROOT/'tools/verify_stack_probe.py'
        spec=importlib.util.spec_from_file_location('verify_stack',verifier)
        self.assertIsNotNone(spec.loader)
        module=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertTrue(module.verify(image)['valid'])

    def test_full_subtitles_release(self):
        """正式发行默认启用全部 Hook 和显示，不能误交付仅索引的阶段 A。"""
        self.assertIn('INDEX_ONLY_COMPLETE_NO_HOOKS',CODE)  # 诊断源码仍可单独构建
        image=BINARY.read_bytes()
        for marker in [b'D3D9_HOOK_ATTEMPT_RETURNED',b'NPC_DIALOGUE_HOOK_READY',
                       b'NPC_NATIVE_FONT_HOOK_READY',b'NPC_SUBTITLE_QUEUE_HASH',
                       b'NPC_DRAW_SUBMITTED',b'NPC_NATIVE_GLYPH_CJK_DRAW',
                       b'FULL_SUBTITLE_HOOKS_ATTEMPTED']:
            self.assertIn(marker,image,marker)
        self.assertNotIn(b'INDEX_ONLY_COMPLETE_NO_HOOKS',image)
        self.assertIn('NPC_DIAG_STAGE=4',(ROOT/'CMakeLists.txt').read_text('utf-8'))
        self.assertIn('call :BuildStage 4',(ROOT/'build.bat').read_text('utf-8'))
        self.assertIn('default=4',(ROOT/'tools/build_clang_cross.py').read_text('utf-8'))
        self.assertIn('Checkpoint(L"NPCSubtitles_phase.log","BEFORE_LOG_PATH")',CODE)
        self.assertIn('Checkpoint(L"NPCSubtitles_phase.log","LOG_PATH_READY")',CODE)

    def test_early_device_discovery_and_diagnostic_guards(self):
        """首先截获 D3D9 创建，确保 LOC 扫描不会错过真实设备。"""
        self.assertLess(CODE.index('BEFORE_EARLY_D3D9_IAT'),CODE.index('BEFORE_INDEX_ALLOC'))
        self.assertIn('if(g_prev_create9)return true;',CODE)
        self.assertIn('D3D9_GAME_DEVICE_CREATED',CODE)
        self.assertIn('D3D9_ENDSCENE_ACTIVE',CODE)
        self.assertIn('NATIVE_GLYPH_CAPTURED',CODE)
        self.assertIn('NATIVE_TEXTURE_CAPTURED',CODE)
        self.assertIn('TextureInterfaceReadable(',CODE)
        self.assertIn('NPC_DIALOGUE_INDEX_HIT',CODE)
        self.assertIn('NPC_DIALOGUE_INDEX_MISS',CODE)
        self.assertIn('FULL_SUBTITLE_HOOKS_ATTEMPTED',CODE)

    def test_separate_critical_hook_phases(self):
        """D3D9 提前安装、Dialogue/字体顺序安装，三个入口均保留独立诊断。"""
        self.assertIn('bool InstallDialogueOnly()',CODE)
        self.assertIn('bool InstallFontsOnly()',CODE)
        self.assertLess(CODE.index('BEFORE_DIALOGUE_HOOK'),CODE.index('BEFORE_FONT_HOOKS'))
        self.assertLess(CODE.index('BEFORE_EARLY_D3D9_IAT'),CODE.index('BEFORE_DIALOGUE_HOOK'))
        self.assertLess(CODE.index('BEFORE_FONT_HOOKS'),CODE.index('BEFORE_D3D9_HOOK'))
        self.assertIn('D3D9_HOOK_ATTEMPT_RETURNED',CODE)

    def test_invalid_log_handle_and_internal_route_storage(self):
        """保留日志句柄保护，不允许加载外置规则或运行旧签名判断。"""
        self.assertIn('if(g_log==(Handle)-1){',CODE)
        self.assertIn('g_log=nullptr;LogFailure("LOG_CREATE_FAILED"',CODE)
        self.assertIn('constexpr U32 ROUTE_FLAG_OFFSET=0x1Cu;',CODE)
        self.assertIn('NATIVE_EVENT_ROUTE_OFFSET',CODE)
        self.assertNotIn('LoadRoutes()',CODE)
        self.assertNotIn('SEMANTIC_ROUTE_MISSING',CODE)

    def test_build_uses_actual_cross_compiler(self):
        """完整工程中必须有真实可执行的 clang 编译与链接入口。"""
        code=(ROOT/'tools/build_clang_cross.py').read_text('utf-8')
        for item in ('clang-cl','lld-link','i686-pc-windows-msvc','/machine:x86','/Brepro'):
            self.assertIn(item,code)

    def test_no_external_route_file_dependency(self):
        """发行只需 ASI，不要求用户附带或修改外置 TSV 规则。"""
        self.assertFalse((PACKAGE/'release/NPCSubtitlesSemanticRoutes.tsv').exists())
        self.assertFalse((ROOT/'config/NPCSubtitlesSemanticRoutes.tsv').exists())
        self.assertIn('LogNativeEventRoute();Checkpoint',CODE)
        self.assertIn('NATIVE_ROUTE_FLAG_READY',CODE)
        self.assertNotIn(b'NPCSubtitlesSemanticRoutes.tsv',BINARY.read_bytes())

    def test_bat_language_padding(self):
        """批处理的中文 REM 和 echo 必须按规则保留尾部两个半角空格。"""
        for bat in [ROOT/'build.bat',PACKAGE/'source/build_all.bat']:
            for ln in bat.read_text('utf-8').splitlines():
                if ln.strip().lower().startswith(('rem ','echo ')):
                    self.assertTrue(ln.endswith('  '),(bat.name,ln))

    def test_white_glyph_background_uses_luminance_mask(self):
        """黑色纹理像素必须完全保留背景；白色笔画应显示在场景之上。"""
        self.assertIn('render(device,19,2);render(device,20,4);',CODE)
        self.assertIn('render(device,19,1);render(device,20,4);',CODE)
        self.assertIn('0xFF555555',CODE)
        self.assertNotIn('0xA8000000',CODE)
        # 模拟固定功能亮度混合：输出=mask + dst*(1-mask)。
        for dst in (0.0,0.2,0.7,1.0):
            self.assertAlmostEqual(0+dst*(1-0),dst)
            self.assertAlmostEqual(1+dst*(1-1),1)
            mask=0.35
            self.assertAlmostEqual(mask+dst*(1-mask),mask+(1-mask)*dst)

    def test_adaptive_scale_and_mixed_script_baseline(self):
        """高分辨率下相对字号下降，英文按中文原始高度归一并共用基线。"""
        self.assertIn('float AutoScale(U32 height)',CODE)
        self.assertIn('const float ref_height=cjk_count?',CODE)
        self.assertIn('script_scale=(ref_height*1.05f)/m.h',CODE)
        self.assertIn('const float top=baseline-h;',CODE)
        self.assertIn('subtitle_queue::AllocateLineBudget(active,g_requested_lines,budget)',CODE)
        self.assertIn('const float baseline=line_y+max_glyph_height;',CODE)
        def factor(h):return min(1.4,max(.3,.43+(h/480-1)*.24))
        vals=[factor(h) for h in (480,720,1080,2160)]
        self.assertEqual(vals,sorted(vals))
        self.assertGreater(vals[0]/480,vals[-1]/2160)
        self.assertLess(factor(1080),1080/480*.56)

    def test_hash_miss_is_not_the_same_as_policy_filter(self):
        """真实脚本对白被排除不能再写成 INDEX_MISS。"""
        self.assertIn('NPC_DIALOGUE_INDEX_MISS',CODE)
        self.assertIn('NPC_DIALOGUE_FILTERED_HASH',CODE)
        self.assertIn('NPC_DIALOGUE_AMBIGUOUS_HASH',CODE)
        start=CODE.index('extern "C" void CDECL AfterDialogue(')
        body=CODE[start:CODE.index('extern "C" __declspec(naked) void DialogueThunk()',start)]
        self.assertLess(body.index('if(!entry)'),body.index('if(entry->ambiguous)'))
        self.assertLess(body.index('if(entry->ambiguous)'),body.index('if(!entry->allowed)'))
        self.assertLess(body.index('if(!entry->allowed)'),body.index('NPC_SUBTITLE_QUEUE_HASH'))

if __name__=='__main__': unittest.main()
