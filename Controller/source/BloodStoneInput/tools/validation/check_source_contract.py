#!/usr/bin/env python3
"""检查 JoyShim 的配置归属与七入口统一接管契约。"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]

def read(path: str) -> str:
    file = ROOT / path
    if file.suffix.lower() == ".bat":
        return file.read_bytes().decode("cp936")
    return file.read_text(encoding="utf-8")

failures: list[str] = []
cmake = read("CMakeLists.txt")
config_h = read("src/config.hpp")
config_cpp = read("src/config.cpp")
controller_match_h = read("src/controller_match.hpp")
controller_match_cpp = read("src/controller_match.cpp")
controller_match_test = read("tests/test_controller_match.cpp")
config_scope_test = read("tests/test_config_scope.cpp")
proxy_h = read("src/proxy_entry.hpp")
proxy_entry = read("src/proxy_entry.cpp")
proxy_system = read("src/proxy_system.cpp")
runtime_start = read("src/runtime_start.hpp")
xinput_proxy = read("src/xinput_proxy.cpp")
directinput = read("src/directinput_filter.cpp")
system_exports = read("src/system_exports.cpp")
toml_reader = read("src/toml_reader.cpp")
default_h = read("src/default_config.hpp")
generic = read("config/template.toml")
bloodstone = read("config/examples/Bond.winmm.toml")
config_check = read("tools/src/config_check.cpp")
probe = read("tools/src/proxy_entry_probe.cpp")
package_msvc = read("tools/build_release.py")
package_mingw = read("tools/build_release.py")

checks = [
    (cmake, "project(JoyShim VERSION 0.5.2", "CMake 内部项目名/版本错误"),
    (cmake, "add_library(JoyShimCore OBJECT", "缺少共享 JoyShimCore 对象核心"),
    (cmake, "JOYSHIM_LAZY_BOOTSTRAP_ONLY", "缺少 DLL 启动崩溃隔离构建选项"),
    (cmake, "joyshim_add_proxy(JoyShimXInput11", "缺少 xinput1_1 独立目标"),
    (cmake, "joyshim_add_proxy(JoyShimXInput12", "缺少 xinput1_2 独立目标"),
    (cmake, "joyshim_add_proxy(JoyShimXInput13", "缺少 xinput1_3 独立目标"),
    (cmake, "joyshim_add_proxy(JoyShimXInput14", "缺少 xinput1_4 独立目标"),
    (cmake, "joyshim_add_proxy(JoyShimXInput910", "缺少 xinput9_1_0 独立目标"),
    (cmake, "joyshim_add_proxy(JoyShimDInput8", "缺少 dinput8 独立目标"),
    (cmake, "joyshim_add_proxy(JoyShimWinMM", "缺少 winmm 独立目标"),
    (config_h, "namespace joyshim", "配置代码未使用 joyshim 命名空间"),
    (config_h, "struct ProjectConfig", "缺少单游戏 Project 配置模型"),
    (config_h, "std::vector<ProcessNameValue> processNames", "Project.ProcessNames 不是多值数组"),
    (config_h, "ProjectConfig project", "RuntimeConfig 未保存当前项目配置"),
    (config_h, "projectPatchApplied", "设备配置未记录主项目父集合应用状态"),
    (config_h, "controllerPatchApplied", "设备配置未记录手柄子集合覆盖状态"),
    (controller_match_h, "std::vector<DeviceIdValue> deviceIds", "DeviceIds 不是成对多值数组"),
    (controller_match_h, "std::vector<std::array<char, 33>> guids", "Guids 不支持多个值"),
    (controller_match_h, "GuidFallback = 6", "Guids 不是固定最后识别层"),
    (controller_match_h, "std::vector<DevicePathValue> paths", "Paths 不支持多个值"),
    (controller_match_cpp, "Serial", "缺少 Serials 第一层匹配"),
    (controller_match_cpp, "Path", "缺少 Paths 第二层匹配"),
    (controller_match_cpp, "DeviceId", "缺少 DeviceIds 型号匹配"),
    (controller_match_cpp, "NameContains", "缺少名称片段回退"),
    (controller_match_cpp, "GuidFallback", "缺少 GUID 最后兜底"),
    (controller_match_cpp, "绝不把 SDL GUID 反解成 VID/PID", "DirectInput 仍可能伪造 GUID 命中"),
    (controller_match_test, "controller.guids.push_back", "未测试多个 GUID"),
    (controller_match_test, "ControllerMatchKind::GuidFallback", "未测试 GUID 最后兜底"),
    (cmake, "add_executable(test_config_scope", "缺少 Windows 配置父子作用域测试目标"),
    (config_scope_test, "LoadDeviceProfile(config, genericIdentity)", "未测试未匹配手柄继承主项目设置"),
    (config_scope_test, "LoadDeviceProfile(config, stadiaIdentity)", "未测试匹配手柄子集合覆盖"),
    (config_scope_test, "ResolveDirectInputPolicy", "未测试 DirectInput 父子作用域合成"),
    (config_scope_test, "子集合未填写，继续继承 true", "未测试字段级稀疏继承"),
    (config_cpp, 'entry.key == "Paths"', "配置解析器缺少 Paths 数组"),
    (config_h, "ExactProcessProfile", "缺少精确 EXE 专版来源状态"),
    (config_h, "SecondaryProcessProfile", "缺少同游戏多 EXE 专版来源状态"),
    (config_cpp, "BuildModuleConfigStem", "配置未使用实际代理 DLL 主名"),
    (config_cpp, "BuildProxyProfileSuffix", "专版文件名没有关联具体代理 DLL"),
    (config_cpp, "BuildExactProfileName", "未实现 <EXE主名>.<代理DLL主名>.toml 精确查找"),
    (config_cpp, "FindFirstFileW", "未实现同目录专版扫描"),
    (config_cpp, "ProcessNamesContain", "未验证专版文件归属与 ProcessNames"),
    (config_cpp, "matches > 1", "未拒绝多个专版同时归属当前进程"),
    (config_cpp, 'key == "Process"', "未明确拒绝旧 Process 单值字段"),
    (config_cpp, 'key == "ProcessContains"', "未明确拒绝模糊进程字段"),
    (config_cpp, "ApplyProfilePatch(profile, config.project.patch)", "主项目父集合设置未先应用"),
    (config_cpp, "ApplyProfilePatch(profile, selectedController->patch)", "手柄子集合设置未覆盖主项目"),
    (config_cpp, 'path[0] == "Project"', "缺少可选 Project.ProcessNames 解析"),
    (config_cpp, "IsProfileSuffix(path)", "主项目功能表不能直接位于顶层"),
    (config_cpp, "旧的 Games 多游戏层级已移除", "未明确拒绝旧 Games 层级"),
    (toml_reader, "文件不是有效 UTF-8", "TOML 解析器未严格校验 UTF-8"),

    # 七入口统一启动与接管。
    (proxy_h, "ProxyEntryInstallEarlyHooks", "缺少主 EXE 最小早期接管入口"),
    (proxy_h, "ProxyEntryQueueRuntimeBootstrap", "缺少延迟无条件运行时启动入口"),
    (proxy_h, "ProxyEntryConfigureHooks", "缺少统一入口接管配置函数"),
    (proxy_h, "ProxyEntryRefreshHooks", "缺少后加载游戏模块刷新函数"),
    (proxy_entry, "RuntimeBootstrapWorker", "缺少 loader lock 外延迟启动线程"),
    (proxy_entry, "DeferredBootstrapDelayMs", "无条件启动线程没有固定延迟保护"),
    (proxy_entry, "ProxyEntryInstallEarlyHooks", "缺少最小早期 IAT 接管实现"),
    (proxy_entry, "allowResolvedAddress", "早期接管无法禁止 OriginalFirstThunk=0 地址解析"),
    (proxy_entry, "CreateThread", "未排队无条件 JoyShim 启动线程"),
    (proxy_entry, "PatchNormalImports", "缺少普通导入接管"),
    (proxy_entry, "OriginalFirstThunk", "缺少普通导入名称表处理"),
    (proxy_entry, "ReplacementByResolvedAddress", "缺少 OriginalFirstThunk=0 回退"),
    (proxy_entry, "PatchDelayImports", "缺少保留的延迟导入诊断实现"),
    (proxy_entry, "JoyShimGetProcAddress", "缺少受控动态解析接管"),
    (proxy_entry, "CreateToolhelp32Snapshot", "缺少后加载游戏模块扫描"),
    (proxy_entry, "IsGameModulePath", "模块扫描没有限制到游戏目录"),
    (proxy_entry, "HookXInput", "所有入口未统一启用 XInput 接管"),
    (proxy_entry, "HookDirectInput", "配置启用时未接入 DirectInput 去重"),
    (proxy_entry, "DirectInput8Create", "未识别 DirectInput8Create"),
    (proxy_entry, "GetProcAddress", "未识别动态 GetProcAddress"),
    (proxy_entry, "xinput1_1.dll", "缺少 xinput1_1 识别"),
    (proxy_entry, "xinput1_2.dll", "缺少 xinput1_2 识别"),
    (proxy_entry, "xinput1_3.dll", "缺少 xinput1_3 识别"),
    (proxy_entry, "xinput1_4.dll", "缺少 xinput1_4 识别"),
    (proxy_entry, "xinput9_1_0.dll", "缺少 xinput9_1_0 识别"),
    (proxy_entry, "dinput8.dll", "缺少 dinput8 识别"),
    (proxy_entry, "winmm.dll", "缺少 winmm 识别"),
    (proxy_system, "ProxyEntryQueueRuntimeBootstrap();", "系统代理调用仍可能同步重入重型初始化"),
    (xinput_proxy, "ProxyEntryProcessAttach(module);", "DllMain 未记录代理入口"),
    (proxy_entry, "ProxyEntryInstallEarlyHooks();", "loader lock 外的工作线程未执行早期 IAT 接管"),
    (xinput_proxy, "ProxyEntryQueueRuntimeBootstrap();", "标准构建缺少延迟 SDL 启动"),
    (xinput_proxy, "ProxyEntryConfigureHooks", "配置解析后未进入统一接管链"),
    (xinput_proxy, "ProxyEntryRefreshHooks(config)", "输入线程未刷新后加载模块"),
    (xinput_proxy, "启动时未连接手柄；输入线程保持运行并等待热插拔", "未保证零设备启动"),
    (xinput_proxy, "XInput 调用已进入 JoyShim", "缺少 XInput 实际执行日志"),
    (xinput_proxy, "JoyShimGetCapabilitiesEx", "缺少 XInput 1.4 @108 私有实现"),
    (read("src/xinput_exports.hpp"), "JoyShimXInputCapabilitiesEx", "缺少 XInput 1.4 扩展能力结构"),
    (directinput, "ConfiguredDirectInput8Create", "缺少按配置透明/包装路由的 DirectInput 入口"),
    (directinput, "ConfigRequestsDirectInputInterception", "DirectInput 早期接管未按实际配置决定是否包装"),
    (directinput, "ProxyEntryNotifyDirectInputCreate", "缺少 DirectInput8Create 实际执行统计"),
    (directinput, "ProxyEntryNotifyDirectInputDeviceHidden", "缺少实际隐藏设备统计"),
    (directinput, "DirectInput8Create 已进入 JoyShim 包装层", "缺少 DirectInput 实际执行日志"),
    (system_exports, "JOYSHIM_System_DirectInput8Create", "dinput8 直接包装入口缺失"),
    (probe, "SDL3 静态链接：不使用外置 SDL3.dll", "入口探针仍采用过时的动态 SDL 判断"),
    (package_msvc, "check_alias_outputs.py", "MSVC 打包未验证七个独立二进制"),
    (package_mingw, "check_alias_outputs.py", "MinGW 打包未验证七个独立二进制"),
    (runtime_start, "初始化不以是否已连接手柄为条件", "运行时接口未声明无条件 SDL 初始化"),
    (config_check, "Bond.winmm.toml", "配置检查工具未说明专版文件验证方式"),
]

for text, token, message in checks:
    if token not in text:
        failures.append(message)

# 公开 XInput API 的异常规范必须和操作系统头文件一致。
# SDK 新版可能自行声明扩展能力结构，此时禁止使用同名 C 函数实现。
xinput_h = read("src/xinput_exports.hpp")
if re.search(r'extern\s+"C"\s+DWORD\s+WINAPI\s+XInputGetCapabilitiesEx\s*\(', xinput_h + xinput_proxy):
    failures.append("扩展能力入口使用系统同名 C 符号，可能导致 MinGW-w64 类型冲突")
if not re.search(r'extern\s+"C"\s+DWORD\s+WINAPI\s+JoyShimGetCapabilitiesEx\s*\(', xinput_h):
    failures.append("缺少独立于系统头文件的扩展能力内部声明")
if not re.search(r'extern\s+"C"\s+DWORD\s+WINAPI\s+JoyShimGetCapabilitiesEx\s*\(', xinput_proxy):
    failures.append("缺少独立于系统头文件的扩展能力实现")
if re.search(r'extern\s+"C"[^;{]*\bXInput(?:Get|Set|Enable|Wait|Cancel|PowerOff)[A-Za-z]*\s*\([^;{]*\)\s+noexcept', xinput_h + xinput_proxy):
    failures.append("公开 XInput C 函数附加了系统头文件没有的 noexcept")
if '&JoyShimGetCapabilitiesEx' not in xinput_proxy:
    failures.append("XInput 扩展能力动态解析仍使用系统声明或错误符号")

aliases = (
    "xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll",
    "xinput9_1_0.dll", "dinput8.dll", "winmm.dll",
)
# 构建器从两个不同架构的编译目录收集七个独立 DLL，
# 统一双架构验证已由 check_build_scripts.py 负责，本检查只确认代理架构契约。
for alias in aliases:
    if alias.removesuffix(".dll") not in package_msvc:
        failures.append(f"发行构建器缺少入口：{alias}")

if "joyshim_add_proxy(JoyShimVersion" in cmake or "Version," in read("src/proxy_entry.hpp"):
    failures.append("version.dll 代理仍被注册")
if "bool blockDirectInput{false}" not in config_h or "bool enabled = false;" not in config_cpp:
    failures.append("DirectInput 运行时默认值没有统一为 false")

# 静态 CRT 需要线程通知；不得再次调用 DisableThreadLibraryCalls。
if re.search(r"\bDisableThreadLibraryCalls\s*\(", xinput_proxy):
    failures.append("静态 CRT DLL 不得调用 DisableThreadLibraryCalls")

# DllMain 只允许记录入口、排队不等待的启动线程和发送退出事件。
# 严禁在 loader lock 下安装早期 Hook、加载 SDL、
# 扫描后加载模块、处理延迟导入或等待。
match = re.search(
    r'extern "C" BOOL WINAPI JoyShimExportedDllMain\([\s\S]*?\n}\n\nextern "C" BOOL WINAPI DllMain',
    xinput_proxy,
)
if not match:
    failures.append("无法提取 JoyShimExportedDllMain 进行 loader-lock 审计")
else:
    body = match.group(0)
    for forbidden in (
        "LoadRuntimeConfig", "PatchCurrentGameModules", "CreateToolhelp32Snapshot",
        "LoadLibrary", "WaitForSingleObject", "SDL_", "ProxyEntryInstallEarlyHooks();",
    ):
        if forbidden in body:
            failures.append(f"DllMain 中禁止出现重型操作：{forbidden}")

# 最小接管函数只能从 loader lock 外的 RuntimeBootstrapWorker 调用。
worker = re.search(r'DWORD WINAPI RuntimeBootstrapWorker\(void\*\) noexcept \{[\s\S]*?\n\}', proxy_entry)
if not worker or 'ProxyEntryInstallEarlyHooks();' not in worker.group(0):
    failures.append('工作线程未执行延后 IAT 接管')
if 'bool RuntimeReadyOrQueue() noexcept' not in xinput_proxy:
    failures.append('XInput 导出没有非阻塞启动检查')
if re.search(r'if \(\!EnsureInitialized\(\)\)', xinput_proxy):
    failures.append('XInput 导出仍在同步等待 SDL 初始化')

# 最小早期接管只允许轻量 IAT 处理，不允许启动期重型扫描。
early_match = re.search(
    r'void ProxyEntryInstallEarlyHooks\(\) noexcept \{[\s\S]*?\n\}',
    proxy_entry,
)
if not early_match:
    failures.append("无法提取 ProxyEntryInstallEarlyHooks 进行早期接管审计")
else:
    early_body = early_match.group(0)
    for forbidden in (
        "LoadRuntimeConfig", "LoadLibrary", "CreateToolhelp32Snapshot",
        "PatchDelayImports", "EnsureRuntimeInitialized", "SDL_",
    ):
        if forbidden in early_body:
            failures.append(f"最小早期接管中禁止出现重型操作：{forbidden}")
    for required in ("GetModuleHandleW(nullptr)", "false, false"):
        if required not in early_body:
            failures.append(f"最小早期接管缺少安全边界：{required}")

# 系统代理导出可能从其他 DLL 的 DllMain 中调用；不得在这里同步等待完整初始化。
if "EnsureRuntimeInitialized();" in proxy_system:
    failures.append("系统 API 转发路径不得同步执行完整 JoyShim 初始化")

for path in list((ROOT / "src").glob("**/*")) + list((ROOT / "tools").glob("**/*")) + list((ROOT / "tests").glob("**/*")):
    if not path.is_file() or path.suffix.lower() not in {".cpp", ".hpp", ".h", ".cmake"}:
        continue
    text = path.read_text(encoding="utf-8", errors="ignore")
    for forbidden in ("namespace simple_s2input", "S2I_", "SimpleS2inputExportedDllMain"):
        if forbidden in text:
            failures.append(f"{path.relative_to(ROOT)} 仍含旧内部标识：{forbidden}")

for text, label in ((generic, "template.toml"), (bloodstone, "Bond.winmm.toml")):
    if "Process =" in text or "ProcessContains" in text:
        failures.append(f"{label} 仍使用旧的单值/模糊进程字段")

for token, message in (
    ('[NativeInput]', "Blood Stone 专版缺少主项目 NativeInput 表"),
    ('HideDirectInput = true', "Blood Stone 专版未对当前游戏全部手柄启用 DirectInput 去重"),
    ('[Controllers.Stadia]', "Blood Stone 专版缺少自定义 Stadia 手柄子集合"),
    ('DeviceIds = [', "Blood Stone Stadia 子集合未使用多值 DeviceIds"),
    ('Guids = [', "Blood Stone Stadia 子集合未保留多 GUID 最后兜底能力"),
):
    if token not in bloodstone:
        failures.append(message)

for text, label in ((generic, "template.toml"), (bloodstone, "Bond.winmm.toml")):
    if "[Games." in text or ".Common." in text:
        failures.append(f"{label} 仍包含旧 Games/Common 层级")

for token in ("GameProfileConfig", "GameControllerRuleConfig", "config.games"):
    if token in config_h or token in config_cpp or token in xinput_proxy:
        failures.append(f"运行时仍保留旧多游戏配置模型：{token}")

if "Bond.exe" in config_cpp or "BloodStone" in config_cpp or "18D1" in config_cpp:
    failures.append("通用配置核心仍硬编码 Blood Stone/Stadia")
if "GuidVendorProduct" in config_cpp:
    failures.append("DirectInput 仍在把 SDL GUID 反解成 VID/PID")

start = default_h.find('R"JOYSHIMTOML(')
end = default_h.find(')JOYSHIMTOML";', start)
if start < 0 or end < 0:
    failures.append("无法提取内嵌配置模板")
elif default_h[start + len('R"JOYSHIMTOML('):end] != generic:
    failures.append("内嵌通用模板与 template.toml 不一致")

if "add_library(JoyShim SHARED" in cmake or 'OUTPUT_NAME "JoyShim"' in cmake:
    failures.append("仍保留旧单一胖 DLL 构建目标")
for obsolete in ("exports_x86_mingw.def", "exports_x64_mingw.def", "exports_msvc_private_x86.def", "exports_msvc_private_x64.def"):
    if (ROOT / "src" / obsolete).exists():
        failures.append(f"仍保留旧胖 DLL 导出定义：{obsolete}")

if failures:
    print("源码契约检查失败：", file=sys.stderr)
    for item in failures:
        print(f"- {item}", file=sys.stderr)
    raise SystemExit(1)

print("JoyShim 单游戏父集合/手柄子集合、七入口接管、配置归属与热插拔契约检查通过。")
