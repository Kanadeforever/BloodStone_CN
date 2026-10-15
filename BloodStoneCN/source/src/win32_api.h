#pragma once

// 本头文件只声明当前插件真正用到的 Windows Win32 函数。
// 这样可在没有完整 Windows SDK 的交叉构建环境下编译相同源码。
// 所有函数与类型都遵循 32 位 Windows ABI，不能用于 64 位游戏。

using DWord = unsigned long;
using Bool = int;
using Handle = void*;
using Module = void*;
using VoidPointer = void*;

#define WIN_CALL __stdcall
#define IMPORT_API __declspec(dllimport)

extern "C" {
IMPORT_API Module WIN_CALL GetModuleHandleW(const wchar_t* name);
IMPORT_API const wchar_t* WIN_CALL GetCommandLineW();
IMPORT_API DWord WIN_CALL GetTickCount();
IMPORT_API DWord WIN_CALL GetCurrentProcessId();
IMPORT_API DWord WIN_CALL GetLastError();
IMPORT_API int WIN_CALL MultiByteToWideChar(unsigned int codepage, DWord flags, const char* input, int length, wchar_t* output, int capacity);
IMPORT_API int WIN_CALL WideCharToMultiByte(unsigned int codepage, DWord flags,
    const wchar_t* source, int length, char* output, int output_capacity,
    const char* default_char, Bool* used_default);
IMPORT_API DWord WIN_CALL GetShortPathNameW(const wchar_t* long_path, wchar_t* short_path, DWord capacity);
IMPORT_API DWord WIN_CALL GetModuleFileNameW(Module module, wchar_t* name, DWord capacity);
IMPORT_API DWord WIN_CALL GetFileAttributesW(const wchar_t* path);
IMPORT_API Bool WIN_CALL VirtualProtect(VoidPointer address, DWord size, DWord protection, DWord* old_protection);
IMPORT_API Handle WIN_CALL GetCurrentProcess();
IMPORT_API Bool WIN_CALL FlushInstructionCache(Handle process, const void* address, DWord size);
IMPORT_API void WIN_CALL OutputDebugStringA(const char* message);
IMPORT_API Handle WIN_CALL CreateThread(void* security, DWord stack_size,
    DWord (WIN_CALL* start)(void*), void* arg, DWord flags, DWord* id);
IMPORT_API void WIN_CALL Sleep(DWord milliseconds);
IMPORT_API Handle WIN_CALL CreateFileW(const wchar_t* path, DWord access, DWord share,
    void* security, DWord disposition, DWord flags, Handle template_file);
IMPORT_API Bool WIN_CALL WriteFile(Handle file, const void* bytes, DWord size,
    DWord* written, void* overlapped);
IMPORT_API Bool WIN_CALL ReadFile(Handle file, void* buffer, DWord size,
    DWord* read, void* overlapped);
IMPORT_API Bool WIN_CALL CloseHandle(Handle file);
IMPORT_API Bool WIN_CALL FlushFileBuffers(Handle file);
IMPORT_API DWord WIN_CALL GetFileSize(Handle file, DWord* high_part);

// 用于只读遍历外置 LOC 目录。结构体字段大小与 Win32 的 WIN32_FIND_DATAW 一致。
struct FileTime { DWord low; DWord high; };
struct FindDataW {
    DWord attributes;
    FileTime created;
    FileTime accessed;
    FileTime modified;
    DWord size_high;
    DWord size_low;
    DWord reserved0;
    DWord reserved1;
    wchar_t file_name[260];
    wchar_t alternate_name[14];
};
IMPORT_API Handle WIN_CALL FindFirstFileW(const wchar_t* pattern, FindDataW* data);
IMPORT_API Bool WIN_CALL FindNextFileW(Handle search, FindDataW* data);
IMPORT_API Bool WIN_CALL FindClose(Handle search);
}

constexpr DWord kDllAttach = 1;
constexpr DWord kPageReadWrite = 0x04;
constexpr DWord kInvalidAttributes = 0xfffffffful;
constexpr DWord kDirectoryAttribute = 0x10;
constexpr DWord kWriteAccess = 0x40000000ul;
constexpr DWord kReadShare = 0x00000001ul;
constexpr DWord kWriteShare = 0x00000002ul;
constexpr DWord kCreateAlways = 2;
constexpr DWord kOpenExisting = 3;
constexpr DWord kReadAccess = 0x80000000ul;
constexpr DWord kReparseAttribute = 0x400ul;
constexpr DWord kUtf8CodePage = 65001;
constexpr DWord kNormalAttributes = 0x80;

// 修改执行区中的 push imm32 字符串地址时，页面需临时拥有执行与写入权限。
constexpr DWord kPageExecuteReadWrite = 0x40;

// 使用进程系统 ANSI 代码页进行无损转换，拒绝任何字符替换。
constexpr DWord kAnsiCodePage = 0;
constexpr DWord kNoBestFitChars = 0x00000400ul;
