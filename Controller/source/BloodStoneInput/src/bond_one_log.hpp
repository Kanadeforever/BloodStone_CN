#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace bondone {
// 入口阶段只确定路径并创建同名日志；每个进程启动覆盖旧文件。
void OpenSessionLog(HMODULE module) noexcept;
// 输入线程和图标线程共用一个串行写入通道，不存在异步日志重排。
void WriteLog(const wchar_t* message) noexcept;
void CloseSessionLog() noexcept;
}
