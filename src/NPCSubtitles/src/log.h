#pragma once
#include "common.h"
namespace npcsub {
void LogOpen(const std::wstring& module_directory, bool enabled);
void LogClose();
void LogLine(const char* level, const char* fmt, ...);
}
#define NPC_LOG_INFO(...) ::npcsub::LogLine("INFO", __VA_ARGS__)
#define NPC_LOG_WARN(...) ::npcsub::LogLine("WARN", __VA_ARGS__)
#define NPC_LOG_ERROR(...) ::npcsub::LogLine("ERROR", __VA_ARGS__)
