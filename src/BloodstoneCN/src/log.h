#pragma once
#include "common.h"

namespace bscn {

// 运行时日志器。
//
// 不再使用 _wfopen/fopen 持有 CRT stdio 流。Win32 句柄显式允许其他进程/编辑器
// 在游戏运行期间以读方式打开 BloodstoneCN.log；每条日志先完整格式化，再用一次
// WriteFile 追加，避免读端看到半行的 stdio 用户态缓冲。
class Logger {
public:
    bool Open(const std::wstring& path);
    void Close();
    void Write(const char* level, const char* format, ...);

private:
    bool WriteBytesLocked(const void* data, size_t size);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::mutex mutex_;
};

extern Logger g_log;
}  // namespace bscn

#define BSCN_LOG_INFO(...)  ::bscn::g_log.Write("INFO", __VA_ARGS__)
#define BSCN_LOG_WARN(...)  ::bscn::g_log.Write("WARN", __VA_ARGS__)
#define BSCN_LOG_ERROR(...) ::bscn::g_log.Write("ERROR", __VA_ARGS__)
