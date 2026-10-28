#include "mapping_database.hpp"

#include "sdl_backend.hpp"

#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace joyshim {
namespace {

bool WideToUtf8(const wchar_t* source, char* output, int outputCount) noexcept {
    if (source == nullptr || output == nullptr || outputCount <= 0) return false;
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        source,
        -1,
        output,
        outputCount,
        nullptr,
        nullptr);
    if (converted <= 0) {
        output[0] = '\0';
        return false;
    }
    return true;
}

bool Utf8ToWide(const char* source, wchar_t* output, int outputCount) noexcept {
    if (source == nullptr || output == nullptr || outputCount <= 0) return false;
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        source,
        -1,
        output,
        outputCount);
    if (converted <= 0) {
        output[0] = L'\0';
        return false;
    }
    return true;
}

}  // namespace

MappingLoadResult LoadConfiguredMappingDatabase(
    const RuntimeConfig& config,
    SdlBackend& backend) noexcept {
    MappingLoadResult result{};
    result.requested = config.enableExternalDatabase;

    if (!config.enableExternalDatabase || config.mappingDatabasePath[0] == L'\0') {
        std::swprintf(result.detail, 512, L"外部映射数据库未启用。");
        return result;
    }

    const DWORD attributes = GetFileAttributesW(config.mappingDatabasePath);
    result.fileExists = attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    if (!result.fileExists) {
        std::swprintf(
            result.detail,
            512,
            L"未找到外部映射数据库：%ls",
            config.mappingDatabasePath);
        return result;
    }

    result.apiAvailable = backend.CanAddGamepadMappings();
    if (!result.apiAvailable) {
        std::swprintf(result.detail, 512, L"当前 SDL 后端缺少映射数据库加载接口。");
        return result;
    }

    char utf8Path[MAX_PATH * 4]{};
    if (!WideToUtf8(config.mappingDatabasePath, utf8Path, static_cast<int>(sizeof(utf8Path)))) {
        std::swprintf(result.detail, 512, L"外部映射数据库路径无法转换为 UTF-8。");
        return result;
    }

    result.mappingsAdded = backend.AddGamepadMappingsFromFile(utf8Path);
    if (result.mappingsAdded >= 0) {
        std::swprintf(
            result.detail,
            512,
            L"已加载外部映射数据库：%d 条映射，文件=%ls",
            result.mappingsAdded,
            config.mappingDatabasePath);
        return result;
    }

    wchar_t errorText[256]{};
    Utf8ToWide(backend.GetError(), errorText, 256);
    std::swprintf(
        result.detail,
        512,
        L"外部映射数据库加载失败：%ls",
        errorText[0] != L'\0' ? errorText : L"SDL 未提供错误文本");
    return result;
}

}  // namespace joyshim
