#pragma once

#include <windows.h>

namespace joyshim {

enum class SystemDllKind {
    DInput8,
    WinMM,
};

FARPROC ResolveSystemExport(SystemDllKind kind, const char* name) noexcept;

}  // namespace joyshim
