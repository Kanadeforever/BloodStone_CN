#!/usr/bin/env python3
"""无第三方依赖地检查七个独立代理 DLL 的架构、导出表和静态依赖。"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
SYSTEM_EXPORTS_FILE = ROOT / "src/system_exports.cmake"
MACHINES = {0x014C: "x86", 0x8664: "x64"}
ENTRY_NAMES = (
    "xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4", "xinput9_1_0",
    "dinput8", "winmm",
)


def read_cmake_ordered(variable: str) -> list[str]:
    text = SYSTEM_EXPORTS_FILE.read_text(encoding="utf-8")
    match = re.search(rf"set\({re.escape(variable)}\n(.*?)\n\)", text, re.DOTALL)
    if not match:
        raise RuntimeError(f"无法读取 CMake 列表：{variable}")
    return re.findall(r'^\s*"([A-Za-z0-9_]+):\d+"', match.group(1), re.MULTILINE)


DINPUT8_ORDER = read_cmake_ordered("JOYSHIM_DINPUT8_EXPORTS")
WINMM_ORDER = read_cmake_ordered("JOYSHIM_WINMM_EXPORTS")
DINPUT8_NAMED = set(DINPUT8_ORDER)
WINMM_NAMED = set(WINMM_ORDER)
SYSTEM_NAMED = DINPUT8_NAMED | WINMM_NAMED
XINPUT_ALL_NAMED = {
    "DllMain", "XInputGetState", "XInputSetState", "XInputGetCapabilities",
    "XInputEnable", "XInputGetDSoundAudioDeviceGuids",
    "XInputGetBatteryInformation", "XInputGetKeystroke",
    "XInputGetAudioDeviceIds", "XInputGetCapabilitiesEx",
}


@dataclass(frozen=True)
class Section:
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int


class PeFile:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise ValueError("不是 MZ 可执行文件")
        pe_offset = self.u32(0x3C)
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise ValueError("缺少 PE 签名")
        coff = pe_offset + 4
        self.machine = self.u16(coff)
        section_count = self.u16(coff + 2)
        optional_size = self.u16(coff + 16)
        optional = coff + 20
        magic = self.u16(optional)
        if magic == 0x10B:
            directory_offset = optional + 96
        elif magic == 0x20B:
            directory_offset = optional + 112
        else:
            raise ValueError(f"未知可选头格式：0x{magic:04X}")
        self.export_rva = self.u32(directory_offset)
        self.import_rva = self.u32(directory_offset + 8)
        self.sections: list[Section] = []
        section_table = optional + optional_size
        for index in range(section_count):
            base = section_table + index * 40
            self.sections.append(Section(
                virtual_address=self.u32(base + 12),
                virtual_size=self.u32(base + 8),
                raw_offset=self.u32(base + 20),
                raw_size=self.u32(base + 16),
            ))

    def u16(self, offset: int) -> int:
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.data, offset)[0]

    def rva_offset(self, rva: int) -> int:
        for section in self.sections:
            size = max(section.virtual_size, section.raw_size)
            if section.virtual_address <= rva < section.virtual_address + size:
                return section.raw_offset + (rva - section.virtual_address)
        if 0 <= rva < len(self.data):
            return rva
        raise ValueError(f"RVA 无法映射：0x{rva:X}")

    def cstring_rva(self, rva: int) -> str:
        offset = self.rva_offset(rva)
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise ValueError("字符串没有终止符")
        return self.data[offset:end].decode("ascii", errors="replace")

    def exports(self) -> dict[int, str | None]:
        if self.export_rva == 0:
            return {}
        base = self.rva_offset(self.export_rva)
        ordinal_base = self.u32(base + 16)
        function_count = self.u32(base + 20)
        name_count = self.u32(base + 24)
        names_rva = self.u32(base + 32)
        ordinals_rva = self.u32(base + 36)
        names_by_index: dict[int, str] = {}
        names_offset = self.rva_offset(names_rva)
        ordinals_offset = self.rva_offset(ordinals_rva)
        for index in range(name_count):
            name_rva = self.u32(names_offset + index * 4)
            ordinal_index = self.u16(ordinals_offset + index * 2)
            names_by_index[ordinal_index] = self.cstring_rva(name_rva)
        return {
            ordinal_base + index: names_by_index.get(index)
            for index in range(function_count)
        }

    def imports(self) -> list[str]:
        if self.import_rva == 0:
            return []
        result: list[str] = []
        offset = self.rva_offset(self.import_rva)
        while True:
            descriptor = self.data[offset:offset + 20]
            if len(descriptor) < 20 or descriptor == b"\0" * 20:
                break
            result.append(self.cstring_rva(self.u32(offset + 12)))
            offset += 20
        return result


def expected_named(entry: str) -> set[str]:
    if entry in ("xinput1_1", "xinput1_2"):
        return {
            "XInputEnable", "XInputGetCapabilities",
            "XInputGetDSoundAudioDeviceGuids", "XInputSetState", "XInputGetState",
        }
    if entry == "xinput1_3":
        return {
            "DllMain", "XInputGetState", "XInputSetState", "XInputGetCapabilities",
            "XInputEnable", "XInputGetDSoundAudioDeviceGuids",
            "XInputGetBatteryInformation", "XInputGetKeystroke",
        }
    if entry == "xinput1_4":
        return {
            "DllMain", "XInputGetState", "XInputSetState", "XInputGetCapabilities",
            "XInputEnable", "XInputGetBatteryInformation", "XInputGetKeystroke",
            "XInputGetAudioDeviceIds", "XInputGetCapabilitiesEx",
        }
    if entry == "xinput9_1_0":
        return {
            "XInputGetCapabilities", "XInputGetDSoundAudioDeviceGuids",
            "XInputSetState", "XInputGetState",
        }
    if entry == "dinput8":
        return DINPUT8_NAMED
    return WINMM_NAMED


def check_fixed_ordinals(entry: str, exports: dict[int, str | None], failures: list[str]) -> None:
    if entry == "xinput1_3":
        named = {
            1: "DllMain", 2: "XInputGetState", 3: "XInputSetState",
            4: "XInputGetCapabilities", 5: "XInputEnable",
            6: "XInputGetDSoundAudioDeviceGuids",
            7: "XInputGetBatteryInformation", 8: "XInputGetKeystroke",
        }
        noname = {100, 101, 102, 103}
    elif entry == "xinput1_4":
        named = {
            1: "DllMain", 2: "XInputGetState", 3: "XInputSetState",
            4: "XInputGetCapabilities", 5: "XInputEnable",
            7: "XInputGetBatteryInformation", 8: "XInputGetKeystroke",
            10: "XInputGetAudioDeviceIds", 108: "XInputGetCapabilitiesEx",
        }
        noname = {100, 101, 102, 103}
    elif entry in ("xinput1_1", "xinput1_2"):
        named = {
            1: "XInputEnable", 2: "XInputGetCapabilities",
            3: "XInputGetDSoundAudioDeviceGuids", 4: "XInputSetState",
            5: "XInputGetState",
        }
        noname = set()
    elif entry == "xinput9_1_0":
        named = {
            1: "XInputGetCapabilities", 2: "XInputGetDSoundAudioDeviceGuids",
            3: "XInputSetState", 4: "XInputGetState",
        }
        noname = set()
    elif entry == "dinput8":
        # COM 标准入口（DllCanUnloadNow、DllGetClassObject、注册/注销入口）
        # 只要求按名称导出，不固定 ordinal。MSVC 对它们显式指定 ordinal
        # 会产生 LNK4222；DirectInput8Create 与 GetdfDIJoystick 仍固定为 @1/@6。
        named = {
            1: "DirectInput8Create",
            6: "GetdfDIJoystick",
        }
        noname = set()
    else:
        named = {}
        next_ordinal = 1
        reserved = {2, 3, 4}
        for name in WINMM_ORDER:
            if name == "PlaySoundA":
                named[2] = name
                continue
            while next_ordinal in reserved:
                next_ordinal += 1
            named[next_ordinal] = name
            next_ordinal += 1
        noname = set()
    for ordinal, name in named.items():
        if exports.get(ordinal) != name:
            failures.append(
                f"固定序号错误：@{ordinal} 实际 {exports.get(ordinal)!r}，预期 {name!r}")
    for ordinal in noname:
        if ordinal not in exports:
            failures.append(f"缺少隐藏序号：@{ordinal}")
        elif exports[ordinal] is not None:
            failures.append(f"隐藏序号 @{ordinal} 不应具有名称：{exports[ordinal]!r}")


# 系统提供的 DLL 可以正常动态链接；MinGW/第三方运行库不能要求用户
# 额外复制到游戏目录。将阻止列表作为固定发行契约，保持大小写不敏感。
EXTERNAL_RUNTIME_DLLS = {
    "libwinpthread-1.dll", "libstdc++-6.dll", "libgcc_s_dw2-1.dll",
    "libgcc_s_seh-1.dll", "libgcc_s_sjlj-1.dll", "libatomic-1.dll",
    "libssp-0.dll", "libgomp-1.dll", "libquadmath-0.dll",
    "libc++.dll", "libc++abi.dll", "libunwind.dll",
    "msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll",
    "vcruntime140_1.dll", "concrt140.dll",
}


def check_runtime_imports(imports: list[str]) -> list[str]:
    """返回不能出现在正式 JoyShim DLL 导入表中的外置运行库。"""
    return sorted({name for name in imports if name.lower() in EXTERNAL_RUNTIME_DLLS},
                  key=str.lower)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dll", type=Path)
    parser.add_argument("--expect-arch", choices=("x86", "x64"), required=True)
    parser.add_argument("--expect-entry", choices=ENTRY_NAMES, required=True)
    args = parser.parse_args()
    try:
        pe = PeFile(args.dll)
        arch = MACHINES.get(pe.machine, f"0x{pe.machine:04X}")
        exports = pe.exports()
        imports = pe.imports()
    except (OSError, ValueError, struct.error, RuntimeError) as exc:
        print(f"PE 检查失败：{exc}", file=sys.stderr)
        return 1

    failures: list[str] = []
    if arch != args.expect_arch:
        failures.append(f"架构错误：实际 {arch}，预期 {args.expect_arch}")

    actual_named = {name for name in exports.values() if name}
    expected = expected_named(args.expect_entry)
    missing = sorted(expected - actual_named)
    extra = sorted(actual_named - expected)
    if missing:
        failures.append("缺少名称导出：" + ", ".join(missing))
    if extra:
        failures.append("出现不属于该入口的名称导出：" + ", ".join(extra[:16]) + ("..." if len(extra) > 16 else ""))

    if args.expect_entry.startswith("xinput"):
        foreign = actual_named & SYSTEM_NAMED
    else:
        foreign = actual_named & XINPUT_ALL_NAMED
    if foreign:
        failures.append("导出表混入其他代理家族：" + ", ".join(sorted(foreign)))

    check_fixed_ordinals(args.expect_entry, exports, failures)

    normalized_imports = {item.lower() for item in imports}
    external = check_runtime_imports(imports)
    if external:
        failures.append('含有必须额外安装的 MinGW/第三方运行库：' + ', '.join(external))
    if any(name.startswith("xinput") for name in normalized_imports):
        failures.append(f"存在不允许的系统 XInput 静态依赖：{sorted(normalized_imports)}")
    for sdl_name in ("sdl2.dll", "sdl3.dll"):
        if sdl_name in normalized_imports:
            failures.append(f"{sdl_name} 被静态导入；正式版本必须静态链接 SDL3，不允许外置依赖")

    print(f"文件：{args.dll}")
    print(f"入口：{args.expect_entry}")
    print(f"架构：{arch}")
    print(f"名称导出：{len(actual_named)}")
    print("导入 DLL：" + (", ".join(imports) if imports else "<none>"))
    if failures:
        print("检查未通过：", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("独立代理导出表、固定序号和静态依赖检查通过。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
