#include "bond_one_log.hpp"
#include <cwchar>

namespace bondone {
namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_file = INVALID_HANDLE_VALUE;
}
void OpenSessionLog(HMODULE module) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return;
    wchar_t* suffix = std::wcsrchr(path, L'.');
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (suffix == nullptr || (slash != nullptr && suffix < slash)) return;
    // 例如 dinput8.dll 同目录生成 dinput8.log；CREATE_ALWAYS 每次清空旧日志。
    if (static_cast<size_t>(suffix - path) + 5 >= MAX_PATH) return;
    std::wmemcpy(suffix, L".log", 5);
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
    WriteLog(L"Blood Stone 专用输入与图标模块启动；日志已清空。 ");
}
void WriteLog(const wchar_t* message) noexcept {
    if (message == nullptr) return;
    SYSTEMTIME timestamp{};
    GetLocalTime(&timestamp);
    wchar_t wide[3072]{};
    const int count = std::swprintf(wide, 3072,
        L"[%04u-%02u-%02u %02u:%02u:%02u] %ls\r\n",
        timestamp.wYear, timestamp.wMonth, timestamp.wDay,
        timestamp.wHour, timestamp.wMinute, timestamp.wSecond, message);
    if (count <= 0) return;
    char utf8[9216]{};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, count, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes <= 0) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
void CloseSessionLog() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_lock);
}
}
