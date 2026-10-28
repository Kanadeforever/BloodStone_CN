#!/usr/bin/env python3
"""编译头桩对象并确认全部包装器/核心导出具有外部链接。"""

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CLANG = shutil.which("clang++")
NM = shutil.which("nm")
if CLANG is None or NM is None:
    print("缺少 clang++ 或 nm，无法检查对象符号。", file=sys.stderr)
    raise SystemExit(2)

system_list = (ROOT / "src/system_exports.cmake").read_text(encoding="utf-8")
expected_system = set(re.findall(r'^\s*"([A-Za-z0-9_]+):\d+"', system_list, flags=re.MULTILINE))
expected_xinput = {
    "DllMain", "JoyShimExportedDllMain", "XInputGetState", "XInputGetStateEx",
    "XInputSetState", "XInputGetCapabilities", "JoyShimGetCapabilitiesEx", "XInputEnable",
    "XInputGetDSoundAudioDeviceGuids", "XInputGetAudioDeviceIds",
    "XInputGetBatteryInformation", "XInputGetKeystroke",
    "XInputWaitForGuideButton", "XInputCancelGuideButtonWait",
    "XInputPowerOffController",
}

common = [
    CLANG, "-std=c++17", "-fms-extensions", "-D_WIN32",
    f"-I{ROOT / 'tests/windows_stub/include'}", f"-I{ROOT / 'src'}",
    "-Wall", "-Wextra", "-Wpedantic", "-Wcast-function-type", "-Werror", "-c",
]

with tempfile.TemporaryDirectory(prefix="joyshim-symbols-") as temp:
    temp_path = Path(temp)
    objects = {
        "system": (ROOT / "src/system_exports.cpp", temp_path / "system_exports.o"),
        "xinput": (ROOT / "src/xinput_proxy.cpp", temp_path / "xinput_proxy.o"),
    }
    symbols: dict[str, set[str]] = {}
    for key, (source, obj) in objects.items():
        subprocess.run(common + [str(source), "-o", str(obj)], cwd=ROOT, check=True)
        output = subprocess.check_output([NM, "-g", "--defined-only", str(obj)], text=True)
        symbols[key] = {
            line.split()[-1]
            for line in output.splitlines()
            if line.strip() and len(line.split()) >= 3
        }

actual_system = {
    symbol.removeprefix("JOYSHIM_System_")
    for symbol in symbols["system"]
    if symbol.startswith("JOYSHIM_System_")
}
if actual_system != expected_system:
    print(
        "系统包装器外部符号不一致："
        f"缺少={sorted(expected_system - actual_system)}，"
        f"多余={sorted(actual_system - expected_system)}",
        file=sys.stderr,
    )
    raise SystemExit(1)
missing_xinput = expected_xinput - symbols["xinput"]
if missing_xinput:
    print(f"XInput 外部符号缺少：{sorted(missing_xinput)}", file=sys.stderr)
    raise SystemExit(1)

print(f"对象外部链接检查通过：{len(actual_system)} 个系统包装器，{len(expected_xinput)} 个 XInput/入口符号。")
