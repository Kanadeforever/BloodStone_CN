#!/usr/bin/env python3
"""检查七个独立代理目标的导出定义和 C++ 包装器一致性。"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
system_cmake = (ROOT / "src/system_exports.cmake").read_text(encoding="utf-8")
system_cpp = (ROOT / "src/system_exports.cpp").read_text(encoding="utf-8")
failures: list[str] = []

ENTRIES = (
    "xinput1_1", "xinput1_2", "xinput1_3", "xinput1_4", "xinput9_1_0",
    "dinput8", "winmm",
)
PRIVATE_SYSTEM = {"DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer", "DllUnregisterServer"}


def cmake_entries(variable: str) -> dict[str, int]:
    match = re.search(rf"set\({re.escape(variable)}\n(.*?)\n\)", system_cmake, re.DOTALL)
    if not match:
        failures.append(f"缺少导出清单：{variable}")
        return {}
    pairs = re.findall(r'^\s*"([A-Za-z0-9_]+):(\d+)"', match.group(1), re.MULTILINE)
    result = {name: int(arity) for name, arity in pairs}
    if len(result) != len(pairs):
        failures.append(f"{variable} 存在重复名称")
    return result


dinput = cmake_entries("JOYSHIM_DINPUT8_EXPORTS")
winmm = cmake_entries("JOYSHIM_WINMM_EXPORTS")
dinput_order = list(dinput)
winmm_order = list(winmm)
expected_system = dinput | winmm
if len(dinput) != 6:
    failures.append(f"dinput8 名称导出数量错误：{len(dinput)}")
if len(winmm) != 185:
    failures.append(f"winmm 名称导出数量错误：{len(winmm)}")
if len(expected_system) != 191:
    failures.append(f"系统代理名称导出总数不是 191：{len(expected_system)}")

cpp_entries = {
    name: int(arity)
    for arity, _kind, name in re.findall(
        r"JOYSHIM_PROXY_(\d+)\((DInput8|WinMM),\s*([A-Za-z0-9_]+)\)",
        system_cpp,
    )
}
if "JOYSHIM_System_DirectInput8Create(" in system_cpp:
    cpp_entries["DirectInput8Create"] = 5
if cpp_entries != expected_system:
    missing = sorted(set(expected_system) - set(cpp_entries))
    extra = sorted(set(cpp_entries) - set(expected_system))
    wrong = sorted(
        name for name in set(expected_system) & set(cpp_entries)
        if expected_system[name] != cpp_entries[name]
    )
    failures.append(
        f"C++ 包装器与清单不一致：缺少={missing}，多余={extra}，参数数错误={wrong}")

for token in (
    "add_library(JoyShimCore OBJECT",
    "joyshim_add_proxy(JoyShimXInput11",
    "joyshim_add_proxy(JoyShimXInput12",
    "joyshim_add_proxy(JoyShimXInput13",
    "joyshim_add_proxy(JoyShimXInput14",
    "joyshim_add_proxy(JoyShimXInput910",
    "joyshim_add_proxy(JoyShimDInput8",
    "joyshim_add_proxy(JoyShimWinMM",
    'set(export_definition "${CMAKE_CURRENT_SOURCE_DIR}/src/exports_${export_id}_x86.def")',
    'set(export_definition "${CMAKE_CURRENT_SOURCE_DIR}/src/exports_${export_id}_x64.def")',
    'joyshim_prepare_export_definition(selected_definition',
    'target_sources(${target} PRIVATE "${selected_definition}")',
):
    if token not in cmake:
        failures.append(f"独立代理构建缺少：{token}")


def parse_def(path: Path) -> tuple[dict[str, str], dict[str, int], set[str], set[str]]:
    aliases: dict[str, str] = {}
    ordinals: dict[str, int] = {}
    noname: set[str] = set()
    private: set[str] = set()
    for line in path.read_text(encoding="ascii").splitlines():
        match = re.match(
            r"\s*([A-Za-z0-9_]+)=([^\s]+)(?:\s+@(\d+))?(?:\s+(NONAME))?(?:\s+(PRIVATE))?\s*$",
            line,
        )
        if not match:
            continue
        public, internal, ordinal, noname_flag, private_flag = match.groups()
        aliases[public] = internal
        if ordinal:
            ordinals[public] = int(ordinal)
        if noname_flag:
            noname.add(public)
        if private_flag:
            private.add(public)
    return aliases, ordinals, noname, private


def expected_internal(name: str, arity: int, x86: bool, prefix: str = "JOYSHIM_System_") -> str:
    symbol = prefix + name
    return f"_{symbol}@{arity * 4}" if x86 else symbol

xinput_expected = {
    "xinput1_1": {"XInputEnable", "XInputGetCapabilities", "XInputGetDSoundAudioDeviceGuids", "XInputSetState", "XInputGetState"},
    "xinput1_2": {"XInputEnable", "XInputGetCapabilities", "XInputGetDSoundAudioDeviceGuids", "XInputSetState", "XInputGetState"},
    "xinput1_3": {"DllMain", "XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputEnable", "XInputGetDSoundAudioDeviceGuids", "XInputGetBatteryInformation", "XInputGetKeystroke", "XInputGetStateEx", "XInputWaitForGuideButton", "XInputCancelGuideButtonWait", "XInputPowerOffController"},
    "xinput1_4": {"DllMain", "XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputEnable", "XInputGetBatteryInformation", "XInputGetKeystroke", "XInputGetAudioDeviceIds", "XInputGetStateEx", "XInputWaitForGuideButton", "XInputCancelGuideButtonWait", "XInputPowerOffController", "XInputGetCapabilitiesEx"},
    "xinput9_1_0": {"XInputGetCapabilities", "XInputGetDSoundAudioDeviceGuids", "XInputSetState", "XInputGetState"},
}

for entry in ENTRIES:
    for arch in ("x86", "x64"):
        path = ROOT / "src" / f"exports_{entry}_{arch}.def"
        if not path.is_file():
            failures.append(f"缺少导出定义：{path.name}")
            continue
        aliases, ordinals, noname, private = parse_def(path)
        names = set(aliases)
        if entry in xinput_expected:
            expected_names = xinput_expected[entry]
        elif entry == "dinput8":
            expected_names = set(dinput)
        else:
            expected_names = set(winmm)
        if names != expected_names:
            failures.append(
                f"{path.name} 名称集合错误：缺少={sorted(expected_names - names)}，多余={sorted(names - expected_names)}")

        x86 = arch == "x86"
        # XInput 1.4 的扩展入口使用私有内部符号，避免和新版 MinGW-w64
        # 中的同名 C 声明发生类型冲突。公开名称与固定序号保持不变。
        if entry == "xinput1_4":
            expected_alias = ("_JoyShimGetCapabilitiesEx@16" if x86
                              else "JoyShimGetCapabilitiesEx")
            actual_alias = aliases.get("XInputGetCapabilitiesEx")
            if actual_alias != expected_alias:
                failures.append(
                    f"{path.name} 扩展能力入口内部符号错误："
                    f"{actual_alias!r}，预期 {expected_alias!r}")
        if entry in ("dinput8", "winmm"):
            arities = dinput if entry == "dinput8" else winmm
            for name, arity in arities.items():
                expected = expected_internal(name, arity, x86)
                if aliases.get(name) != expected:
                    failures.append(
                        f"{path.name} 别名错误：{name} -> {aliases.get(name)!r}，预期 {expected!r}")
                    break
        if entry == "dinput8" and private != PRIVATE_SYSTEM:
            failures.append(f"{path.name} PRIVATE 集合错误：{sorted(private)}")
        if entry != "dinput8" and private and entry not in ("xinput1_3", "xinput1_4"):
            failures.append(f"{path.name} 出现意外 PRIVATE 导出：{sorted(private)}")

        if entry in ("xinput1_1", "xinput1_2"):
            expected_ord = {
                "XInputEnable": 1, "XInputGetCapabilities": 2,
                "XInputGetDSoundAudioDeviceGuids": 3,
                "XInputSetState": 4, "XInputGetState": 5,
            }
            expected_noname = set()
        elif entry == "xinput1_3":
            expected_ord = {
                "DllMain": 1, "XInputGetState": 2, "XInputSetState": 3,
                "XInputGetCapabilities": 4, "XInputEnable": 5,
                "XInputGetDSoundAudioDeviceGuids": 6,
                "XInputGetBatteryInformation": 7, "XInputGetKeystroke": 8,
                "XInputGetStateEx": 100, "XInputWaitForGuideButton": 101,
                "XInputCancelGuideButtonWait": 102, "XInputPowerOffController": 103,
            }
            expected_noname = {"XInputGetStateEx", "XInputWaitForGuideButton", "XInputCancelGuideButtonWait", "XInputPowerOffController"}
        elif entry == "xinput1_4":
            expected_ord = {
                "DllMain": 1, "XInputGetState": 2, "XInputSetState": 3,
                "XInputGetCapabilities": 4, "XInputEnable": 5,
                "XInputGetBatteryInformation": 7, "XInputGetKeystroke": 8,
                "XInputGetAudioDeviceIds": 10, "XInputGetStateEx": 100,
                "XInputWaitForGuideButton": 101, "XInputCancelGuideButtonWait": 102,
                "XInputPowerOffController": 103, "XInputGetCapabilitiesEx": 108,
            }
            expected_noname = {"XInputGetStateEx", "XInputWaitForGuideButton", "XInputCancelGuideButtonWait", "XInputPowerOffController"}
        elif entry == "xinput9_1_0":
            expected_ord = {
                "XInputGetCapabilities": 1,
                "XInputGetDSoundAudioDeviceGuids": 2,
                "XInputSetState": 3,
                "XInputGetState": 4,
            }
            expected_noname = set()
        elif entry == "dinput8":
            # COM 标准入口必须按名称导出，不能在 .def 中显式指定 ordinal；
            # 否则 MSVC 会触发 LNK4222，并在 /WX 下终止链接。
            # DirectInput8Create 与 GetdfDIJoystick 保留系统兼容所需的固定序号。
            expected_ord = {
                "DirectInput8Create": 1,
                "GetdfDIJoystick": 6,
            }
            expected_noname = set()
        else:
            expected_ord = {}
            next_ordinal = 1
            reserved = {2, 3, 4}
            for name in winmm_order:
                if name == "PlaySoundA":
                    expected_ord[name] = 2
                    continue
                while next_ordinal in reserved:
                    next_ordinal += 1
                expected_ord[name] = next_ordinal
                next_ordinal += 1
            expected_noname = set()
        if ordinals != expected_ord:
            failures.append(f"{path.name} 固定序号错误：{ordinals}")
        if noname != expected_noname:
            failures.append(f"{path.name} NONAME 集合错误：{sorted(noname)}")

for obsolete in (
    "exports_version_x86.def", "exports_version_x64.def",
    "exports_x86_mingw.def", "exports_x64_mingw.def",
    "exports_msvc_private_x86.def", "exports_msvc_private_x64.def",
):
    if (ROOT / "src" / obsolete).exists():
        failures.append(f"仍保留旧胖 DLL 导出定义：{obsolete}")

if failures:
    print("导出契约检查失败：", file=sys.stderr)
    for item in failures:
        print(f"- {item}", file=sys.stderr)
    raise SystemExit(1)
print("七个独立代理目标、XInput 版本序号与 191 个系统名称导出定义检查通过。")
