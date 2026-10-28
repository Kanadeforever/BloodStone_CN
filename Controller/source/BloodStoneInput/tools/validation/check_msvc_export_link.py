#!/usr/bin/env python3
"""用 clang-cl/lld-link 对七个独立 MSVC x86/x64 导出表执行真实 PE 桩链接。"""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CLANG_CL = shutil.which("clang-cl")
LLD_LINK = shutil.which("lld-link")
if CLANG_CL is None or LLD_LINK is None:
    print("缺少 clang-cl 或 lld-link，无法执行完整 MSVC 导出桩链接。", file=sys.stderr)
    raise SystemExit(2)

system_text = (ROOT / "src/system_exports.cmake").read_text(encoding="utf-8")
SYSTEM_EXPORTS = [
    (name, int(arity))
    for name, arity in re.findall(r'^\s*"([A-Za-z0-9_]+):(\d+)"', system_text, flags=re.MULTILINE)
]
PRIVATE = {"DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer", "DllUnregisterServer"}
XINPUT_TARGETS = [
    ("JoyShimExportedDllMain", 3),
    ("XInputGetState", 2),
    ("XInputSetState", 2),
    ("XInputGetCapabilities", 3),
    ("JoyShimGetCapabilitiesEx", 4),
    ("XInputEnable", 1),
    ("XInputGetDSoundAudioDeviceGuids", 3),
    ("XInputGetBatteryInformation", 3),
    ("XInputGetKeystroke", 3),
    ("XInputGetStateEx", 2),
    ("XInputWaitForGuideButton", 3),
    ("XInputCancelGuideButtonWait", 1),
    ("XInputPowerOffController", 1),
    ("XInputGetAudioDeviceIds", 5),
]
ENTRIES = (
    "xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4", "xinput9_1_0",
    "dinput8", "winmm",
)


def function_source(name: str, arity: int, prefix: str = "") -> str:
    args = ", ".join(f"uptr a{index}" for index in range(1, arity + 1)) or "void"
    unused = " ".join(f"(void)a{index};" for index in range(1, arity + 1))
    return f"uptr JOYSHIM_CALL {prefix}{name}({args}) {{ {unused} return 0; }}"


with tempfile.TemporaryDirectory(prefix="joyshim-msvc-export-") as temp:
    temp_path = Path(temp)
    for architecture, target in (
        ("x86", "i686-pc-windows-msvc"),
        ("x64", "x86_64-pc-windows-msvc"),
    ):
        source = temp_path / f"{architecture}.c"
        obj = temp_path / f"{architecture}.obj"
        integer_type = "unsigned long" if architecture == "x86" else "unsigned long long"
        lines = [
            "#ifdef _M_IX86",
            "#define JOYSHIM_CALL __stdcall",
            "#else",
            "#define JOYSHIM_CALL",
            "#endif",
            f"typedef {integer_type} uptr;",
        ]
        lines.extend(function_source(name, arity) for name, arity in XINPUT_TARGETS)
        lines.extend(function_source(name, arity, "JOYSHIM_System_") for name, arity in SYSTEM_EXPORTS)
        source.write_text("\n".join(lines) + "\n", encoding="utf-8")
        subprocess.run(
            [CLANG_CL, f"--target={target}", "/nologo", "/c", "/GS-", "/Zl", f"/Fo:{obj}", str(source)],
            check=True,
        )

        for entry in ENTRIES:
            dll = temp_path / f"{entry}-{architecture}.dll"
            response = temp_path / f"{entry}-{architecture}.rsp"
            definition = ROOT / "src" / f"exports_{entry}_{architecture}.def"
            response.write_text(
                "\n".join([
                    "/dll",
                    "/noentry",
                    "/nodefaultlib",
                    "/wx",
                    f"/machine:{architecture}",
                    f"/out:{dll}",
                    str(obj),
                    f"/def:{definition}",
                ]) + "\n",
                encoding="utf-8",
            )
            subprocess.run([LLD_LINK, f"@{response}"], check=True)
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/validation/inspect_pe.py"),
                    str(dll),
                    "--expect-arch", architecture,
                    "--expect-entry", entry,
                ],
                check=True,
            )
            if entry == "dinput8":
                import_library = dll.with_suffix(".lib").read_bytes()
                leaked = sorted(name for name in PRIVATE if name.encode("ascii") in import_library)
                if leaked:
                    print(
                        f"{architecture} dinput8 导入库错误包含 PRIVATE 名称：{leaked}",
                        file=sys.stderr,
                    )
                    raise SystemExit(1)
            print(f"{architecture} / {entry} 独立导出桩链接通过。")
