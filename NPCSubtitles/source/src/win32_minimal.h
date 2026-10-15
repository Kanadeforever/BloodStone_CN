#pragma once
// 只声明 NPC 字幕所需的 Win32 和 Direct3D9 ABI，避免构建端依赖本机 SDK。
// 结构尺寸、调用约定、虚表索引必须与 x86 游戏一致；此文件不含游戏修改逻辑。
using U8 = unsigned char;
using U16 = unsigned short;
using U32 = unsigned long;
using I32 = long;
using U64 = unsigned long long;
using WChar = wchar_t;
using Ptr = void*;
using Handle = void*;
using DWORD = unsigned long;
using BOOL = int;
using HRESULT = long;
using UINT = unsigned int;
#define WINAPI __stdcall
#define FASTCALL __fastcall
#define CDECL __cdecl
#define DLLIMPORT __declspec(dllimport)
constexpr U32 kInvalidFile = 0xFFFFFFFF;
extern "C" {
DLLIMPORT void* WINAPI GetModuleHandleW(const wchar_t*);
DLLIMPORT void* WINAPI GetProcAddress(void*,const char*);
DLLIMPORT U32 WINAPI GetModuleFileNameW(void*,wchar_t*,U32);
DLLIMPORT const wchar_t* WINAPI GetCommandLineW();
DLLIMPORT U32 WINAPI GetCurrentThreadId();
DLLIMPORT U32 WINAPI GetLastError();
DLLIMPORT U32 WINAPI GetFileAttributesW(const wchar_t*);
DLLIMPORT Handle WINAPI CreateFileW(const wchar_t*,U32,U32,void*,U32,U32,Handle);
DLLIMPORT BOOL WINAPI ReadFile(Handle,void*,U32,U32*,void*);
DLLIMPORT BOOL WINAPI WriteFile(Handle,const void*,U32,U32*,void*);
DLLIMPORT BOOL WINAPI CloseHandle(Handle);
DLLIMPORT U32 WINAPI GetFileSize(Handle,U32*);
DLLIMPORT BOOL WINAPI VirtualProtect(void*,U32,U32,U32*);
DLLIMPORT Handle WINAPI GetCurrentProcess();
DLLIMPORT BOOL WINAPI ReadProcessMemory(Handle,const void*,void*,U32,U32*);
DLLIMPORT BOOL WINAPI FlushInstructionCache(Handle,const void*,U32);
DLLIMPORT void* WINAPI VirtualAlloc(void*,U32,U32,U32);
DLLIMPORT BOOL WINAPI VirtualFree(void*,U32,U32);
DLLIMPORT U32 WINAPI GetTickCount();
DLLIMPORT void WINAPI GetLocalTime(void*);
DLLIMPORT void WINAPI Sleep(U32);
DLLIMPORT Handle WINAPI CreateThread(void*,U32,U32 (WINAPI*)(void*),void*,U32,U32*);
DLLIMPORT BOOL WINAPI DisableThreadLibraryCalls(void*);
DLLIMPORT void WINAPI OutputDebugStringA(const char*);
DLLIMPORT void* WINAPI FindFirstFileW(const wchar_t*,void*);
DLLIMPORT BOOL WINAPI FindNextFileW(void*,void*);
DLLIMPORT BOOL WINAPI FindClose(void*);
}
// SYSTEMTIME 的 8 个 WORD 字段共 16 字节，与 Win32 API 写入布局相同。
struct SystemTime {U16 year,month,day_of_week,day,hour,minute,second,milliseconds;};
static_assert(sizeof(SystemTime)==16,"本地时间结构必须为 16 字节");
struct FileTime { U32 low, high; };
struct FindDataW { U32 attributes; FileTime created,accessed,modified; U32 high,low,reserved0,reserved1; wchar_t name[260],short_name[14]; };
struct Viewport { U32 x,y,width,height; float zmin,zmax; };
struct SurfaceDesc { U32 format,type,usage,pool,multisample,quality,width,height; };
struct PresentParams { U32 width,height,format,back_buffer_count,multisample,quality,swap_effect; void* window; BOOL windowed,auto_depth; U32 auto_depth_format,flags,full_screen_refresh,presentation_interval; };
struct Vertex { float x,y,z,rhw; U32 color; float u,v; };
struct Glyph { void* vtable; U16 codepoint,pad; float left,mid,right,u0,v0,u1,v1; void* font; };
static_assert(sizeof(void*)==4,"本插件只支持 Win32/x86");
static_assert(sizeof(FindDataW)==592,"Win32 文件枚举结果结构必须为 592 字节");
static_assert(sizeof(Glyph)==40,"字体字形结构偏移不正确");
static_assert(sizeof(PresentParams)==56,"D3D9 PRESENT 参数结构偏移不正确");
