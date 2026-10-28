#!/usr/bin/env python3
"""验证专用单 DLL 的条件编译、花括号和 XInput ABI 重声明。"""

from pathlib import Path
import re
import shutil
import subprocess
import sys

# 脚本随源码携带，不依赖当前工作目录，也不写入游戏或发行目录。
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "src"
STUBS = ROOT / "tests/windows_stub/include"


def ensure(condition: bool, message: str) -> None:
    """发现结构错误立即失败，避免让编译器产生数百条后续报错。"""
    if not condition:
        raise RuntimeError(message)


def selected_function(source: str, start: str, stop: str, single_dll: bool) -> str:
    """仅展开指定函数中的专版分支，以复查不同构建模式的闭合花括号。"""
    first = source.index(start)
    last = source.index(stop, first)
    block = source[first:last]
    output: list[str] = []
    # 此处函数只有一个专版条件分支；不处理任何其他 C++ 预处理指令。
    selected = True
    inside = False
    for line in block.splitlines():
        directive = line.strip()
        if directive == "#ifdef BSCN_SINGLE_DLL":
            ensure(not inside, "检查函数包含意外的嵌套专版条件")
            inside = True
            selected = single_dll
            continue
        if directive == "#else" and inside:
            selected = not selected
            continue
        if directive == "#endif" and inside:
            inside = False
            selected = True
            continue
        if selected:
            output.append(line)
    ensure(not inside, "条件编译缺少 #endif")
    return "\n".join(output)


def check_braces(code: str, label: str) -> None:
    """去掉注释和常见字符串后，检查括号是否提前关闭或遗漏。"""
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)
    depth = 0
    for character in code:
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            ensure(depth >= 0, f"{label}：存在未匹配的结束花括号")
    ensure(depth == 0, f"{label}：缺少 {depth} 枚闭合花括号")


def check_icon_entry_syntax(icons: str, compiler: str) -> None:
    """分别编译专版入口与独立 ASI 入口，防止条件编译多出花括号。"""
    # 仅提取末尾的入口函数，不复制整份 D3D9/游戏逆向代码。
    # 所有占位声明只用于本地验证 C++ 语法，不参与正式 DLL 构建。
    marker = "namespace {\n\nvoid InitializeBondIconRuntime(HMODULE module)"
    ensure(marker in icons, "没有找到图标运行时独立初始化函数")
    suffix = icons[icons.rfind(marker):]
    header = r'''
        #include <cstdint>
        using HMODULE = void*;
        using HANDLE = void*;
        using LPVOID = void*;
        using DWORD = unsigned long;
        using BOOL = int;
        #define APIENTRY
        #define FALSE 0
        #define TRUE 1
        #define DLL_PROCESS_ATTACH 1
        #define DLL_PROCESS_DETACH 0
        extern HMODULE g_selfModule;
        extern HMODULE g_mainModule;
        extern uintptr_t g_base;
        extern DWORD g_threadRuntimeTlsIndex;
        extern bool g_threadLibraryCallsDisabled;
        extern long g_workerStopRequested;
        extern int g_logLock;
        extern int g_textureLock;
        extern bool g_iniPresent;
        extern wchar_t g_iniPath[];
        extern int g_enableLog;
        DWORD TlsAlloc();
        BOOL DisableThreadLibraryCalls(HMODULE);
        long InterlockedExchange(long*, long);
        void InitializeCriticalSection(int*);
        void DeleteCriticalSection(int*);
        void InitializePaths();
        int GetPrivateProfileIntW(const wchar_t*, const wchar_t*, int, const wchar_t*);
        void OpenLog();
        void CloseLog();
        void WriteFinalStatistics();
        void HideOwnRuntimeImportDirectories(HMODULE);
        HMODULE GetModuleHandleW(const wchar_t*);
        bool InstallEarlyDirect3DCreate9Hook();
        void Log(const wchar_t*, ...);
        DWORD WorkerThread(LPVOID);
        HANDLE CreateThread(void*, int, DWORD(*)(LPVOID), LPVOID, int, void*);
        BOOL CloseHandle(HANDLE);
    '''
    for single_dll in (False, True):
        flags = ["-DBSCN_SINGLE_DLL"] if single_dll else []
        result = subprocess.run(
            [compiler, "-std=c++17", "-fsyntax-only", "-x", "c++", "-",
             "-Wall", "-Wextra", "-Werror", *flags],
            input=header + "\n" + suffix,
            capture_output=True, text=True, cwd=ROOT,
        )
        ensure(result.returncode == 0,
               f"{'专版' if single_dll else '独立 ASI'} 入口语法检查失败：\n{result.stderr}")
    print("[通过] 图标模块初始化、独立 DllMain 与专版 C 接口：两种条件编译均通过 C++17 语法检查")


def main() -> None:
    icons = (SOURCES / "gamepad_icons/BondGamepadIcons.cpp").read_text(encoding="utf-8")
    for single_dll in (False, True):
        code = selected_function(
            icons,
            "ResolvedControllerFamily ResolveAutomaticControllerFamily()",
            "void PublishAutomaticControllerFamily(",
            single_dll,
        )
        check_braces(code, "单 DLL" if single_dll else "独立图标模块")
    print("[通过] 自动图标识别函数的两套条件编译分支花括号完整")

    config = (SOURCES / "config.cpp").read_text(encoding="utf-8")
    # 专版 return 与通用实现必须处于互斥的条件编译分支，不能形成不可达代码。
    for start, stop in (
        ("bool ConfigRequestsDirectInputInterception(", "DirectInputPolicyDecision ResolveDirectInputPolicy("),
        ("void WriteLogLine(", "}  // namespace joyshim"),
    ):
        code = config[config.index(start):config.index(stop, config.index(start))]
        ensure("#else" in code and "#endif" in code,
               "专版与通用配置逻辑没有用 #else 隔离")
    print("[通过] 专版配置与日志逻辑不再产生不可达代码")

    # 使用兼容的 Windows ABI 头桩检查两类 SDK 函数签名。
    # 无 Clang 的 Windows 机器可略过此项，其实际 MSVC 构建会做最终验证。
    compiler = shutil.which("clang++")
    if compiler is None:
        print("[提示] 未发现 Clang，仅完成纯源码结构检查")
        return
    common = [
        compiler, "-std=c++17", "-fms-extensions", "-fsyntax-only",
        "-D_WIN32", "-DBSCN_SINGLE_DLL", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", f"-I{STUBS}", f"-I{SOURCES}",
        "-include", str(STUBS / "sdl3_only_type_string.hpp"),
    ]
    # 旧版、MinGW 普通声明、MSVC 新版 noexcept 声明都执行一次语法检查。
    modes = (
        ("旧版 SDK", ["-DJOYSHIM_TEST_LEGACY_XINPUT_SDK"]),
        ("MinGW SDK", []),
        ("MSVC SDK", ["-DJOYSHIM_TEST_MSVC_XINPUT_SDK"]),
    )
    for name, options in modes:
        for filename in ("config.cpp", "xinput_proxy.cpp", "proxy_entry.cpp"):
            command = common + options + [str(SOURCES / filename)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            ensure(result.returncode == 0,
                   f"{name}／{filename} 语法检查失败：\n{result.stderr}")
    print("[通过] 旧版／MinGW／MSVC SDK 三组 XInput ABI 头桩语法检查（9 项）")
    check_icon_entry_syntax(icons, compiler)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError) as exc:
        print(f"[失败] {exc}", file=sys.stderr)
        raise SystemExit(1)
