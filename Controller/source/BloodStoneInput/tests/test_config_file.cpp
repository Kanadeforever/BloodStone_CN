#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "config.hpp"
#include "default_config.hpp"

namespace {

// 将测试数据写入独立临时目录，而不是覆盖游戏旁边的用户配置文件。
// 测试使用真实的 LoadRuntimeConfigFile，因此能发现“默认模板能被 TOML
// 词法器解析，但严格业务校验器仍拒绝它”的问题。
bool WriteText(const wchar_t* path, const char* text, std::size_t length) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(file, text, static_cast<DWORD>(length),
                                   &written, nullptr) != 0 && written == length;
    CloseHandle(file);
    return success;
}

bool Check(bool condition, const char* name) {
    if (!condition) std::printf("[失败] %s\n", name);
    return condition;
}

bool VerifyFile(const wchar_t* path, const char* content, std::size_t length,
                bool expectedValid, bool expectedRumble,
                bool expectedHide, bool expectedLog, const char* name) {
    if (!WriteText(path, content, length)) return Check(false, "无法创建临时 TOML");
    const joyshim::RuntimeConfig config = joyshim::LoadRuntimeConfigFile(
        GetModuleHandleW(nullptr), path);
    bool passed = Check(config.configValid == expectedValid, name);
    if (expectedValid) {
        passed &= Check(config.enableRumble == expectedRumble, "Rumble 绑定到真实运行时");
        passed &= Check(config.enableLog == expectedLog, "Log 绑定到真实运行时");
        passed &= Check(config.project.patch.nativeInput.blockDirectInputConfigured,
                        "隐藏设置成功写入项目父集合");
        passed &= Check(config.project.patch.nativeInput.blockDirectInput == expectedHide,
                        "HideDirectInput 配置值有效");
    } else {
        passed &= Check(config.parseErrorLine != 0, "非法配置报告实际出错行号");
    }
    // 无效 TOML 的错误报告属于本测试的临时产物，离开前主动清理。
    if (config.errorReportPath[0] != L'\0') DeleteFileW(config.errorReportPath);
    return passed;
}

}  // namespace

int main() {
    wchar_t tempRoot[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempRoot) == 0) return 1;
    wchar_t tempDirectory[MAX_PATH]{};
    if (GetTempFileNameW(tempRoot, L"jsc", 0, tempDirectory) == 0) return 1;
    DeleteFileW(tempDirectory);
    if (CreateDirectoryW(tempDirectory, nullptr) == 0) return 1;

    wchar_t path[MAX_PATH]{};
    const int length = std::swprintf(path, MAX_PATH, L"%ls\\test_config_file.toml", tempDirectory);
    if (length < 0 || length >= MAX_PATH) return 1;

    // 第一项直接验证发行包使用的嵌入默认模板，而非维护另一份假样本。
    bool ok = VerifyFile(path, joyshim::DefaultTomlTemplate,
                         joyshim::DefaultTomlTemplateSize,
                         true, true, false, false, "默认模板必须完整通过严格解析");

    // 关闭全部开关时，运行时状态、DirectInput 父集合配置应真实改变。
    constexpr char simplified[] =
        "[Input]\nRumble = false\n"
        "[Compatibility]\nHideDirectInput = false\n"
        "[Debug]\nLog = true\n";
    ok &= VerifyFile(path, simplified, sizeof(simplified) - 1,
                     true, false, false, true, "简化配置关闭振动与隐藏");

    // 旧高级配置必须继续工作，不能为了修复新模板破坏历史游戏档案。
    constexpr char legacy[] =
        "[NativeInput]\nHideDirectInput = false\n"
        "[Global.Diagnostics]\nEnabled = true\n";
    ok &= VerifyFile(path, legacy, sizeof(legacy) - 1,
                     true, true, false, true, "兼容旧版高级配置");

    // 新旧路径表达同一设置时禁止隐式覆盖，避免用户误以为某个选项生效。
    constexpr char conflict[] =
        "[Compatibility]\nHideDirectInput = true\n"
        "[NativeInput]\nBlockDirectInput = false\n";
    ok &= VerifyFile(path, conflict, sizeof(conflict) - 1,
                     false, true, true, false, "拒绝两处重复隐藏配置");

    constexpr char unknown[] = "[Debug]\nUnexpected = true\n";
    ok &= VerifyFile(path, unknown, sizeof(unknown) - 1,
                     false, true, true, false, "严格拒绝未知简化字段");

    // 配置检查器自身不是代理 DLL，因此用显式路径分别验证七种
    // DLL 的同名配置。这样既检查命名规则，也调用真实严格 TOML 解析器。
    constexpr const wchar_t* proxyNames[] = {
        L"xinput1_1", L"xinput1_2", L"xinput1_3", L"xinput1_4",
        L"xinput9_1_0", L"dinput8", L"winmm",
    };
    for (const wchar_t* proxyName : proxyNames) {
        wchar_t proxyPath[MAX_PATH]{};
        const int count = std::swprintf(proxyPath, MAX_PATH,
                                        L"%ls\\%ls.toml", tempDirectory, proxyName);
        if (count <= 0 || count >= MAX_PATH) {
            ok = false;
            continue;
        }
        ok &= VerifyFile(proxyPath, joyshim::DefaultTomlTemplate,
                         joyshim::DefaultTomlTemplateSize,
                         true, true, false, false, "七种代理各自的同名配置可严格解析");
        DeleteFileW(proxyPath);
    }

    // 保留同一游戏多 EXE 的专版能力，但必须包含代理主名。
    // 例如 Bond.winmm.toml 不应被错误归为 winmm.toml 通用配置。
    wchar_t specialPath[MAX_PATH]{};
    const int specialCount = std::swprintf(
        specialPath, MAX_PATH, L"%ls\\Bond.winmm.toml", tempDirectory);
    if (specialCount > 0 && specialCount < MAX_PATH) {
        ok &= VerifyFile(specialPath, joyshim::DefaultTomlTemplate,
                         joyshim::DefaultTomlTemplateSize,
                         true, true, false, false, "代理专用的游戏配置可严格解析");
        DeleteFileW(specialPath);
    } else {
        ok = false;
    }

    // 显式解析时仍以工具自身的模块路径建立默认路径信息。
    // 检查路径末尾必须跟随可执行模块的主名，而不是固定的 joyshim.toml。
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0) {
        wchar_t* extension = std::wcsrchr(modulePath, L'.');
        if (extension != nullptr) {
            const std::size_t remaining = MAX_PATH - static_cast<std::size_t>(extension - modulePath);
            if (remaining < 6) {
                ok = false;
            } else {
                std::wmemcpy(extension, L".toml", 6);
            }
            const auto info = joyshim::LoadRuntimeConfigFile(
                GetModuleHandleW(nullptr), path);
            ok &= Check(_wcsicmp(info.genericTomlPath, modulePath) == 0,
                        "默认配置路径跟随当前模块主名");
            if (info.errorReportPath[0] != L'\0') DeleteFileW(info.errorReportPath);
        }
    }

    DeleteFileW(path);
    RemoveDirectoryW(tempDirectory);
    if (ok) std::puts("[通过] 简化模板、运行时绑定、旧配置与严格错误报告");
    return ok ? 0 : 1;
}
