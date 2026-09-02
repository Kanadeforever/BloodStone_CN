#include "log.h"
#include <cstdarg>
namespace npcsub {
namespace { HANDLE g_file = INVALID_HANDLE_VALUE; std::mutex g_mutex; }
void LogOpen(const std::wstring& module_directory, bool enabled) {
    LogClose();
    if (!enabled) return;
    const std::wstring path = JoinPath(module_directory,L"NPCSubtitles.log");
    g_file = CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
}
void LogClose() { if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; } }
void LogLine(const char* level, const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE || !fmt) return;
    char body[2048]{};
    va_list ap; va_start(ap,fmt); vsnprintf_s(body,sizeof(body),_TRUNCATE,fmt,ap); va_end(ap);
    SYSTEMTIME st{}; GetLocalTime(&st);
    char line[2300]{};
    const int n = snprintf(line,sizeof(line),"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [tid=%lu] %s\r\n",
        st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds,
        level?level:"INFO",GetCurrentThreadId(),body);
    if (n <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    DWORD written=0; WriteFile(g_file,line,static_cast<DWORD>(std::min<int>(n,sizeof(line)-1)),&written,nullptr); FlushFileBuffers(g_file);
}
}
