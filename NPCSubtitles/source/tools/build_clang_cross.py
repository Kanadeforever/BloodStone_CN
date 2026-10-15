# -*- coding: utf-8 -*-
"""用预装 Clang/LLD 直接生成可执行的 Win32/x86 NPCSubtitles.asi。"""
from __future__ import annotations
from pathlib import Path
import subprocess
import shutil
import argparse
ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'source' / 'NPCSubtitles'
WORK = ROOT / '.build' / 'NativeCross'
OUTPUT = ROOT / 'release' / 'NPCSubtitles.asi'
# 名称是 Kernel32.dll 的公开导出；数字是 Win32 x86 stdcall 的参数字节数。
IMPORTS = {
 'GetModuleHandleW':4, 'GetProcAddress':8, 'GetModuleFileNameW':12,
 'GetCommandLineW':0,'GetCurrentThreadId':0,'GetLastError':0,'GetLocalTime':4,
 'GetFileAttributesW':4, 'CreateFileW':28, 'ReadFile':20, 'WriteFile':20,
 'CloseHandle':4, 'GetFileSize':8, 'VirtualProtect':16,
 'GetCurrentProcess':0, 'ReadProcessMemory':20,'FlushInstructionCache':12,
 'VirtualAlloc':16, 'VirtualFree':12, 'GetTickCount':0, 'Sleep':4,
 'CreateThread':24, 'DisableThreadLibraryCalls':4,'OutputDebugStringA':4,
 'FindFirstFileW':8, 'FindNextFileW':8,'FindClose':4,
}
def run(*args:str)->None:
    print('[编译]', ' '.join(args),flush=True)
    subprocess.run(args, check=True)
def main()->None:
    parser=argparse.ArgumentParser(description='离线编译 Win32 NPC 字幕插件，默认构建完整对话、字体和 D3D9 字幕功能候选')
    parser.add_argument('--stage',type=int,choices=(1,2,3,4),default=4,
                        help='明确指定诊断阶段；诊断需求可显式指定 1～3，默认完整功能 4')
    args=parser.parse_args()
    WORK.mkdir(parents=True,exist_ok=True)
    OUTPUT.parent.mkdir(parents=True,exist_ok=True)
    imp=WORK/'kernel32.lib'
    run('lld-link','/lib',f'/def:{SOURCE / "tools/kernel32.def"}', '/machine:x86',f'/out:{imp}')
    # 默认交付完整字幕功能；阶段 1～3 仅供故障定位独立编译。
    # 当前稳定交付是完整 NPC 字幕阶段 4，历史诊断阶段不得覆盖发行版。
    stage=args.stage
    target=OUTPUT if stage==4 else ROOT/'release'/'diagnostics'/f'阶段{stage}'/'NPCSubtitles.asi'
    obj=WORK/f'native_stage_{stage}.obj'
    target.parent.mkdir(parents=True,exist_ok=True)
    aliases=[f'/alternatename:__imp__{name}@{count}=__imp__{name}' for name,count in IMPORTS.items()]
    run('clang-cl','--target=i686-pc-windows-msvc','/nologo','/c','/O2','/GS-','/GR-','/Zl','/EHsc-', '/W3',
        f'/DNPC_DIAG_STAGE={stage}', f'/Fo:{obj}',str(SOURCE/'src/native_standalone.cpp'))
    run('lld-link','/dll','/machine:x86','/Brepro','/entry:DllMain@12', '/nodefaultlib',
        *aliases, f'/out:{target}', f'/implib:{WORK / f"NPCSubtitles_{stage}.lib"}',str(obj),str(imp))
    print('[阶段]',stage,'PE32 DLL',target,target.stat().st_size)
    print('[完成] 已编译指定阶段；默认正式发行包含完整字幕功能阶段 4')
    # 编译对象与导入库都可以从此源码重新产生，成功后清理暂存物。
    shutil.rmtree(WORK)
if __name__ == '__main__': main()
