# -*- coding: utf-8 -*-
"""编译只有 DllMain 日志、没有任何 Hook 的最小 Win32 ASI。"""
from __future__ import annotations

from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "source" / "NPCSubtitles"
OUTPUT = ROOT / "release" / "启动诊断版" / "NPCSubtitles.asi"
WORK = ROOT / ".build" / "NPCBootProbe"


def execute(*args: str) -> None:
    """直接执行交叉编译器，并将任何链接错误反馈给开发者。"""
    print("[诊断版构建]", " ".join(args), flush=True)
    subprocess.run(args, check=True)


def main() -> None:
    """生成可复现 PE32 DLL，不打包用户游戏文件。"""
    WORK.mkdir(parents=True, exist_ok=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    lib = WORK / "kernel32.lib"
    obj = WORK / "startup_probe.obj"
    execute("lld-link", "/lib", f"/def:{MODULE / 'tools' / 'kernel32.def'}", "/machine:x86", f"/out:{lib}")
    execute("clang-cl", "--target=i686-pc-windows-msvc", "/nologo", "/c", "/O2", "/GS-", "/GR-", "/Zl", "/EHsc-",
            f"/Fo:{obj}", str(MODULE / "src" / "startup_probe.cpp"))
    imports = ("GetModuleFileNameW@12", "CreateFileW@28", "WriteFile@20", "CloseHandle@4", "OutputDebugStringA@4")
    aliases = [f"/alternatename:__imp__{name}=__imp__{name.split('@')[0]}" for name in imports]
    execute("lld-link", "/dll", "/machine:x86", "/Brepro", "/entry:DllMain@12", "/nodefaultlib", *aliases,
            f"/implib:{WORK / 'NPCBootProbe.lib'}", f"/out:{OUTPUT}", str(obj), str(lib))
    shutil.rmtree(WORK)
    print("[完成] 无 Hook 启动诊断版：", OUTPUT)


if __name__ == "__main__":
    main()
