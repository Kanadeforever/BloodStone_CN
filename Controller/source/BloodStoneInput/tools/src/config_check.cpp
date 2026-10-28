#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "config.hpp"

namespace {

struct FileStamp {
    FILETIME writeTime{};
    DWORD sizeHigh{};
    DWORD sizeLow{};
    bool valid{};
};

FileStamp QueryFileStamp(const wchar_t* path) noexcept {
    FileStamp stamp{};
    if (path == nullptr || path[0] == L'\0') return stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return stamp;
    stamp.writeTime = data.ftLastWriteTime;
    stamp.sizeHigh = data.nFileSizeHigh;
    stamp.sizeLow = data.nFileSizeLow;
    stamp.valid = true;
    return stamp;
}

bool SameStamp(const FileStamp& left, const FileStamp& right) noexcept {
    return left.valid == right.valid &&
        left.sizeHigh == right.sizeHigh &&
        left.sizeLow == right.sizeLow &&
        left.writeTime.dwHighDateTime == right.writeTime.dwHighDateTime &&
        left.writeTime.dwLowDateTime == right.writeTime.dwLowDateTime;
}

void PrintCaret(std::uint32_t column) noexcept {
    if (column == 0 || column > 240) return;
    for (std::uint32_t index = 1; index < column; ++index) std::fputwc(L' ', stdout);
    std::fputwc(L'^', stdout);
    std::fputwc(L'\n', stdout);
}

const wchar_t* SourceName(joyshim::ConfigSourceKind source) noexcept {
    switch (source) {
        case joyshim::ConfigSourceKind::GeneratedGeneric: return L"新生成的当前目录默认配置";
        case joyshim::ConfigSourceKind::Generic: return L"当前目录默认配置";
        case joyshim::ConfigSourceKind::ExactProcessProfile: return L"精确进程专版";
        case joyshim::ConfigSourceKind::SecondaryProcessProfile: return L"同游戏其他 EXE 专版";
        default: return L"未知";
    }
}

joyshim::RuntimeConfig LoadForCheck(
    HMODULE module,
    const wchar_t* explicitPath,
    const joyshim::RuntimeConfig* current) noexcept {
    if (explicitPath != nullptr && explicitPath[0] != L'\0') {
        return joyshim::LoadRuntimeConfigFile(module, explicitPath);
    }
    if (current != nullptr && current->tomlPath[0] != L'\0') {
        return joyshim::ReloadRuntimeConfig(module, *current);
    }
    return joyshim::LoadRuntimeConfig(module);
}

int PrintValidation(const joyshim::RuntimeConfig& config, bool quietSuccess) noexcept {
    if (config.configValid && !config.configReadFailed) {
        if (!quietSuccess) {
            std::wprintf(
                L"[通过] JoyShim TOML 严格验证通过。\n"
                L"       来源：%ls\n"
                L"       当前进程：%ls\n"
                L"       手柄子集合：%zu，Project.ProcessNames：%zu\n"
                L"       配置文件：%ls\n",
                SourceName(config.configSource),
                config.processName[0] != L'\0' ? config.processName : L"(未确定)",
                config.controllers.size(),
                config.project.processNames.size(),
                config.tomlPath);
        }
        return 0;
    }

    std::wprintf(
        L"[失败] JoyShim TOML 未通过严格验证。\n"
        L"       文件：%ls\n"
        L"       位置：第 %u 行，第 %u 列\n"
        L"       路径：%ls\n"
        L"       错误：%ls\n"
        L"       期望：%ls\n"
        L"       实际：%ls\n"
        L"       建议：%ls\n",
        config.tomlPath,
        static_cast<unsigned int>(config.parseErrorLine),
        static_cast<unsigned int>(config.parseErrorColumn),
        config.parseErrorPath[0] != L'\0' ? config.parseErrorPath : L"(未知)",
        config.parseError[0] != L'\0' ? config.parseError : L"未知错误",
        config.parseErrorExpected[0] != L'\0' ? config.parseErrorExpected : L"(未提供)",
        config.parseErrorActual[0] != L'\0' ? config.parseErrorActual : L"(未提供)",
        config.parseErrorSuggestion[0] != L'\0' ? config.parseErrorSuggestion : L"参考与实际代理 DLL 同名的 TOML 模板");
    if (config.parseErrorSource[0] != L'\0') {
        std::wprintf(L"\n%ls\n", config.parseErrorSource);
        PrintCaret(config.parseErrorColumn);
    }
    std::wprintf(
        L"\n原 TOML 未被修改。完整报告：%ls\n",
        config.errorReportPath[0] != L'\0' ? config.errorReportPath : L"(无法生成)");
    return 1;
}

void PrintUsage() noexcept {
    std::wprintf(
        L"JoyShim Config Check v0.5.2-test12\n"
        L"用法：\n"
        L"  JoyShimConfigCheck.exe\n"
        L"  JoyShimConfigCheck.exe Bond.winmm.toml\n"
        L"  JoyShimConfigCheck.exe --watch [配置文件]\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool watch = false;
    const wchar_t* explicitPath = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], L"--watch") == 0) {
            watch = true;
        } else if (explicitPath == nullptr) {
            explicitPath = argv[index];
        } else {
            PrintUsage();
            return 2;
        }
    }

    HMODULE module = GetModuleHandleW(nullptr);
    joyshim::RuntimeConfig current = LoadForCheck(module, explicitPath, nullptr);
    int lastResult = PrintValidation(current, false);
    if (!watch) return lastResult;

    std::wprintf(
        L"\n监视模式：保存 %ls 后会自动重新验证；按 Ctrl+C 退出。\n\n",
        current.tomlPath[0] != L'\0' ? current.tomlPath : L"配置文件");
    FileStamp previous = QueryFileStamp(current.tomlPath);
    for (;;) {
        Sleep(500);
        const FileStamp nextStamp = QueryFileStamp(current.tomlPath);
        if (SameStamp(previous, nextStamp)) continue;
        previous = nextStamp;
        std::wprintf(L"\n[检测到文件变化，重新验证]\n");
        joyshim::RuntimeConfig next = LoadForCheck(module, explicitPath, &current);
        lastResult = PrintValidation(next, false);
        if (lastResult == 0) {
            current = next;
            std::wprintf(L"配置现在可以使用。\n");
        }
    }
}
