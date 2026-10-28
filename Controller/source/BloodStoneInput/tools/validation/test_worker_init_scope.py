#!/usr/bin/env python3
"""验证 SDL 输入线程初始化失败分支不会引用尚未建立的手柄槽位。"""

from pathlib import Path
import re
import shutil
import subprocess
import sys

# 本脚本放在 tools/validation/ 下，因此向上两级就是 JoyShim 源码根目录。
# 测试直接读取正式运行时代码，不依赖人工复制的测试片段。
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "xinput_proxy.cpp"
STUBS = ROOT / "tests" / "windows_stub" / "include"


def check_failure_scope() -> None:
    """初始化失败时还没有 WorkerSlot，不能读取设备子集合等槽位信息。"""
    source = SOURCE.read_text(encoding="utf-8")

    # 只在 InputWorker 函数内查找失败分支，避免误检查其他函数的同名逻辑。
    worker_start = source.find("DWORD WINAPI InputWorker(")
    if worker_start < 0:
        raise RuntimeError("没有找到 SDL 输入线程入口 InputWorker")
    failure_start = source.find("if (!backend.Initialize(config, detail, 512)) {", worker_start)
    success_start = source.find("joyshim::WriteLogLine(config, detail);", failure_start)
    if failure_start < 0 or success_start < 0 or success_start <= failure_start:
        raise RuntimeError("SDL 初始化失败分支的结构发生变化，需要重新检查测试边界")

    failure_body = source[failure_start:success_start]
    if re.search(r"\bslot\b", failure_body):
        raise RuntimeError("SDL 初始化失败分支引用了尚未建立的 slot")
    if "SetEvent(g_inputReadyEvent);" not in failure_body or "return 0;" not in failure_body:
        raise RuntimeError("SDL 初始化失败时没有正确通知等待线程并退出")


def check_windows_syntax() -> None:
    """借助已有 Windows ABI 测试头检查真实代理代码，尽早发现编译错误。"""
    compiler = shutil.which("clang++")
    if compiler is None:
        # Clang 不是发行构建的强制依赖。没有 Clang 的机器仍然执行上面的
        # 结构检查，然后由 CMake/实际 Windows 编译器完成正式编译验证。
        print("[提示] 未安装 clang++，跳过额外 Windows ABI 语法预检；正式编译继续执行。")
        return

    base = [
        compiler,
        "-std=c++17",
        "-fms-extensions",
        "-fsyntax-only",
        "-D_WIN32",
        f"-I{STUBS}",
        f"-I{ROOT / 'src'}",
        "-Wall", "-Wextra", "-Wpedantic", "-Werror",
    ]
    # 同时验证 x86/x64 与新旧 SDK 路径，不能只检查一种预处理分支。
    checked = 0
    for legacy in (False, True):
        for x64 in (False, True):
            defines = (["-DJOYSHIM_TEST_LEGACY_XINPUT_SDK"] if legacy else [])
            if x64:
                defines.append("-D_WIN64")
            args = base + defines + [str(SOURCE)]
            result = subprocess.run(args, capture_output=True, text=True, errors="replace")
            if result.returncode != 0:
                label = f"{'旧' if legacy else '新'}SDK / {'x64' if x64 else 'x86'}"
                raise RuntimeError(f"Windows ABI 语法检查失败：{label}\n{result.stdout}{result.stderr}")
            checked += 1
    print(f"[通过] 正式代理源码 Windows ABI 严格语法检查：{checked}/{checked}。")


def main() -> int:
    """先运行跨平台结构检查，再根据环境附加真实 C++ 语法检查。"""
    try:
        check_failure_scope()
        check_windows_syntax()
    except (OSError, RuntimeError) as error:
        print(f"[失败] {error}", file=sys.stderr)
        return 1
    print("[通过] SDL 输入线程失败分支不会引用尚未创建的手柄槽位。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
