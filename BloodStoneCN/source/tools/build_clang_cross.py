# -*- coding: utf-8 -*-
"""在非 Windows 主机上交叉编译原生资源目录重定向的 Win32 ASI。"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "source"
WORK = ROOT / ".build" / "BloodstoneCN"
OUTPUT = ROOT / "release" / "Runtime" / "BloodstoneCN.asi"

IMPORTS = {
    "WideCharToMultiByte": 32, "GetShortPathNameW": 12,
    "GetModuleHandleW": 4, "GetModuleFileNameW": 12, "GetFileAttributesW": 4,
    "VirtualProtect": 16, "GetCurrentProcess": 0, "FlushInstructionCache": 12,
    "OutputDebugStringA": 4, "CreateThread": 24, "Sleep": 4,
    "CreateFileW": 28, "WriteFile": 20, "ReadFile": 20, "CloseHandle": 4,
    "GetCommandLineW": 0, "GetTickCount": 0, "GetCurrentProcessId": 0,
    "GetLastError": 0, "MultiByteToWideChar": 24,
    "FlushFileBuffers": 4, "GetFileSize": 8,
    "FindFirstFileW": 8, "FindNextFileW": 8, "FindClose": 4,
}


def execute(*args: str) -> None:
    """命令失败立即抛出异常，防止继续交付上一次遗留的编译产物。"""
    print("[执行]", " ".join(args), flush=True)
    subprocess.run(args, check=True)


def main() -> None:
    WORK.mkdir(parents=True, exist_ok=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    lib = WORK / "kernel32.lib"
    obj = WORK / "bloodstone_cn.obj"
    execute("lld-link", "/lib", f"/def:{SOURCE / 'config' / 'kernel32.def'}", "/machine:x86", f"/out:{lib}")
    execute("clang-cl", "--target=i686-pc-windows-msvc", "/nologo", "/c", "/W4",
            "/WX", "/O2", "/GS-", "/GR-", "/Zl", "/EHsc-",
            f"/Fo:{obj}", str(SOURCE / "src" / "bloodstone_cn.cpp"))
    aliases = [f"/alternatename:__imp__{name}@{count}=__imp__{name}"
               for name, count in IMPORTS.items()]
    execute("lld-link", "/dll", "/machine:x86", "/Brepro", "/entry:DllMain@12",
            "/nodefaultlib", *aliases, f"/implib:{WORK / 'BloodstoneCN.lib'}",
            f"/out:{OUTPUT}", str(obj), str(lib))
    shutil.rmtree(WORK)
    print("[完成] Win32 ASI：", OUTPUT)


if __name__ == "__main__":
    main()
