#include "log.h"

#include <cstdarg>
#include <cstdio>

namespace bscn {
namespace {

bool FormatBody(const char* format, va_list args, std::string* output) {
    if (!format || !output) return false;

    va_list count_args;
    va_copy(count_args, args);
#if defined(_MSC_VER)
    const int required = _vscprintf(format, count_args);
#else
    const int required = vsnprintf(nullptr, 0, format, count_args);
#endif
    va_end(count_args);
    if (required < 0 || required > (1 << 20)) return false;

    std::vector<char> buffer(static_cast<size_t>(required) + 1u, '\0');
    va_list write_args;
    va_copy(write_args, args);
#if defined(_MSC_VER)
    const int written = vsnprintf_s(buffer.data(), buffer.size(), _TRUNCATE,
                                    format, write_args);
#else
    const int written = vsnprintf(buffer.data(), buffer.size(), format,
                                  write_args);
#endif
    va_end(write_args);
    if (written < 0) return false;
    output->assign(buffer.data(), static_cast<size_t>(written));
    return true;
}

}  // namespace

Logger g_log;

bool Logger::Open(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }

    // FILE_SHARE_READ 是用户能够在游戏运行时直接打开日志的关键；同时允许
    // WRITE/DELETE，避免文本编辑器采用“临时文件 + 替换”保存/刷新策略时被
    // BloodstoneCN 阻塞。插件自己仍然只持有一个追加写句柄。
    file_ = CreateFileW(
        path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return file_ != INVALID_HANDLE_VALUE;
}

void Logger::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

bool Logger::WriteBytesLocked(const void* data, size_t size) {
    if (file_ == INVALID_HANDLE_VALUE || !data) return false;
    const uint8_t* current = static_cast<const uint8_t*>(data);
    while (size != 0u) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            size, static_cast<size_t>(0x7FFFFFFFu)));
        DWORD written = 0;
        if (!WriteFile(file_, current, chunk, &written, nullptr) ||
            written != chunk) {
            return false;
        }
        current += written;
        size -= written;
    }
    return true;
}

void Logger::Write(const char* level, const char* format, ...) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) return;

    va_list args;
    va_start(args, format);
    std::string body;
    const bool body_ok = FormatBody(format, args, &body);
    va_end(args);
    if (!body_ok) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char prefix[128]{};
    const int prefix_units = snprintf(
        prefix, sizeof(prefix),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [tid=%lu] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds, level ? level : "INFO",
        static_cast<unsigned long>(GetCurrentThreadId()));
    if (prefix_units <= 0 ||
        static_cast<size_t>(prefix_units) >= sizeof(prefix)) return;

    std::string line;
    line.reserve(static_cast<size_t>(prefix_units) + body.size() + 2u);
    line.append(prefix, static_cast<size_t>(prefix_units));
    line.append(body);
    line.append("\r\n", 2u);

    // 单次完整行写入。WriteFile 没有 CRT stdio 的用户态缓冲，文件尺寸会在
    // 每条日志后立即对共享读句柄可见；这里故意不逐行 FlushFileBuffers，避免
    // 高密度诊断日志触发同步磁盘刷写并造成游戏卡顿。
    WriteBytesLocked(line.data(), line.size());
}

}  // namespace bscn
