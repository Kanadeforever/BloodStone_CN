#pragma once
#include <cstddef>
#include <cstdint>

#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif
#define WINAPI __stdcall
#define CALLBACK __stdcall
#define APIENTRY WINAPI
#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define CP_UTF8 65001
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define FILE_APPEND_DATA 0x0004
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define OPEN_ALWAYS 4
#define CREATE_ALWAYS 2
#define CREATE_NEW 1
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define INFINITE 0xFFFFFFFFu
#define ERROR_SUCCESS 0
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define ERROR_BAD_ARGUMENTS 160
#define ERROR_DEVICE_NOT_CONNECTED 1167
#define ERROR_NOT_SUPPORTED 50
#define ERROR_EMPTY 4306
#define XUSER_MAX_COUNT 4
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define GetFileExInfoStandard 0
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

using BOOL=int;
using BYTE=std::uint8_t;
using WORD=std::uint16_t;
using SHORT=std::int16_t;
using DWORD=std::uint32_t;
using UINT=std::uint32_t;
using ULONG=std::uint32_t;
using ULONG_PTR=std::uintptr_t;
using LONG=std::int32_t;
using ULONGLONG=std::uint64_t;
using LONGLONG=std::int64_t;
using WCHAR=wchar_t;
using LPWSTR=wchar_t*;
using LPCWSTR=const wchar_t*;
using CHAR=char;
using LPCSTR=const char*;
using HANDLE=void*;
using HMODULE=void*;
using HINSTANCE=void*;
using LPVOID=void*;
using LPCVOID=const void*;
using FARPROC=void(*)();
using PVOID=void*;
using SIZE_T=std::size_t;

struct _GUID { std::uint32_t Data1; std::uint16_t Data2,Data3; std::uint8_t Data4[8]; };
using GUID=_GUID;
struct FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; };
struct LARGE_INTEGER { LONGLONG QuadPart; };
struct WIN32_FILE_ATTRIBUTE_DATA { DWORD dwFileAttributes; FILETIME ftCreationTime,ftLastAccessTime,ftLastWriteTime; DWORD nFileSizeHigh,nFileSizeLow; };
struct WIN32_FIND_DATAW { DWORD dwFileAttributes; FILETIME ftCreationTime,ftLastAccessTime,ftLastWriteTime; DWORD nFileSizeHigh,nFileSizeLow; DWORD dwReserved0,dwReserved1; wchar_t cFileName[MAX_PATH]; wchar_t cAlternateFileName[14]; };
struct INIT_ONCE { void* ptr; };
using PINIT_ONCE=INIT_ONCE*;
#define INIT_ONCE_STATIC_INIT {nullptr}
struct SRWLOCK { void* ptr; };
#define SRWLOCK_INIT {nullptr}
using LPTHREAD_START_ROUTINE = DWORD (WINAPI*)(LPVOID);
using PINIT_ONCE_FN = BOOL (CALLBACK*)(PINIT_ONCE, PVOID, PVOID*);

extern "C" {
DWORD GetModuleFileNameW(HMODULE,wchar_t*,DWORD);
DWORD GetPrivateProfileStringW(const wchar_t*,const wchar_t*,const wchar_t*,wchar_t*,DWORD,const wchar_t*);
DWORD GetPrivateProfileSectionNamesW(wchar_t*,DWORD,const wchar_t*);
BOOL SetEnvironmentVariableW(const wchar_t*,const wchar_t*);
HMODULE LoadLibraryW(const wchar_t*);
BOOL FreeLibrary(HMODULE);
HMODULE GetModuleHandleW(const wchar_t*);
FARPROC GetProcAddress(HMODULE,const char*);
DWORD GetLastError();
int MultiByteToWideChar(UINT,DWORD,const char*,int,wchar_t*,int);
HANDLE CreateFileW(const wchar_t*,DWORD,DWORD,void*,DWORD,DWORD,HANDLE);
BOOL WriteFile(HANDLE,const void*,DWORD,DWORD*,void*);
BOOL ReadFile(HANDLE,void*,DWORD,DWORD*,void*);
BOOL GetFileSizeEx(HANDLE,LARGE_INTEGER*);
BOOL FlushFileBuffers(HANDLE);
BOOL DeleteFileW(const wchar_t*);
BOOL CloseHandle(HANDLE);
void Sleep(DWORD);
ULONGLONG GetTickCount64();
BOOL GetFileAttributesExW(const wchar_t*,int,WIN32_FILE_ATTRIBUTE_DATA*);
HANDLE FindFirstFileW(const wchar_t*,WIN32_FIND_DATAW*);
BOOL FindNextFileW(HANDLE,WIN32_FIND_DATAW*);
BOOL FindClose(HANDLE);
HANDLE CreateEventW(void*,BOOL,BOOL,const wchar_t*);
BOOL SetEvent(HANDLE);
DWORD WaitForSingleObject(HANDLE,DWORD);
DWORD WaitForMultipleObjects(DWORD,const HANDLE*,BOOL,DWORD);
HANDLE CreateThread(void*,SIZE_T,LPTHREAD_START_ROUTINE,LPVOID,DWORD,DWORD*);
BOOL InitOnceExecuteOnce(PINIT_ONCE,PINIT_ONCE_FN,PVOID,PVOID*);
void AcquireSRWLockShared(SRWLOCK*);
void ReleaseSRWLockShared(SRWLOCK*);
void AcquireSRWLockExclusive(SRWLOCK*);
void ReleaseSRWLockExclusive(SRWLOCK*);
LONG InterlockedExchange(volatile LONG*,LONG);
LONG InterlockedIncrement(volatile LONG*);
LONG InterlockedDecrement(volatile LONG*);
LONG InterlockedCompareExchange(volatile LONG*,LONG,LONG);
BOOL DisableThreadLibraryCalls(HMODULE);
BOOL SetConsoleOutputCP(UINT);
BOOL SetConsoleCP(UINT);
void OutputDebugStringW(const wchar_t*);
}

using LONG64=std::int64_t;
#define MAXDWORD 0xFFFFFFFFu
#define HEAP_ZERO_MEMORY 0x00000008
#define WT_EXECUTEDEFAULT 0x00000000
struct SYSTEMTIME { WORD wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds; };
extern "C" {
LONG64 InterlockedCompareExchange64(volatile LONG64*,LONG64,LONG64);
HANDLE GetProcessHeap();
LPVOID HeapAlloc(HANDLE,DWORD,SIZE_T);
BOOL HeapFree(HANDLE,DWORD,LPVOID);
void GetLocalTime(SYSTEMTIME*);
int WideCharToMultiByte(UINT,DWORD,const wchar_t*,int,char*,int,const char*,BOOL*);
BOOL QueueUserWorkItem(LPTHREAD_START_ROUTINE,LPVOID,ULONG);
}
#define WC_ERR_INVALID_CHARS 0x80
#define MB_ERR_INVALID_CHARS 0x08
#define FILE_ATTRIBUTE_DIRECTORY 0x10
extern "C" DWORD GetFileAttributesW(const wchar_t*);
#define MAKEINTRESOURCEA(i) ((const char*)((std::uintptr_t)((WORD)(i))))
#define IS_INTRESOURCE(_r) ((((std::uintptr_t)(_r)) >> 16) == 0)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define CP_ACP 0
#define ERROR_PROC_NOT_FOUND 127
struct COORD { SHORT X; SHORT Y; };
extern "C" HANDLE GetStdHandle(DWORD);
extern "C" BOOL SetConsoleCursorPosition(HANDLE,COORD);

#define PAGE_READWRITE 0x04
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

struct IMAGE_DOS_HEADER { WORD e_magic; BYTE pad[58]; LONG e_lfanew; };
struct IMAGE_DATA_DIRECTORY { DWORD VirtualAddress; DWORD Size; };
struct IMAGE_OPTIONAL_HEADER_STUB { DWORD SizeOfImage; IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES]; };
struct IMAGE_NT_HEADERS { DWORD Signature; BYTE fileHeader[20]; IMAGE_OPTIONAL_HEADER_STUB OptionalHeader; };
struct IMAGE_IMPORT_DESCRIPTOR { union { DWORD Characteristics; DWORD OriginalFirstThunk; }; DWORD TimeDateStamp; DWORD ForwarderChain; DWORD Name; DWORD FirstThunk; };
struct IMAGE_THUNK_DATA { union { ULONG_PTR ForwarderString; ULONG_PTR Function; ULONG_PTR Ordinal; ULONG_PTR AddressOfData; } u1; };
struct IMAGE_IMPORT_BY_NAME { WORD Hint; BYTE Name[1]; };
#ifdef _WIN64
#define IMAGE_ORDINAL_FLAG ((ULONG_PTR)0x8000000000000000ull)
#else
#define IMAGE_ORDINAL_FLAG ((ULONG_PTR)0x80000000ul)
#endif
#define IMAGE_SNAP_BY_ORDINAL(Ordinal) (((Ordinal) & IMAGE_ORDINAL_FLAG) != 0)
#define IMAGE_ORDINAL(Ordinal) ((Ordinal) & 0xffff)

extern "C" {
HANDLE GetCurrentProcess();
BOOL VirtualProtect(LPVOID,SIZE_T,DWORD,DWORD*);
BOOL FlushInstructionCache(HANDLE,LPCVOID,SIZE_T);
PVOID InterlockedExchangePointer(PVOID volatile*,PVOID);
UINT GetSystemDirectoryW(wchar_t*,UINT);
int _stricmp(const char*,const char*);
}

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE __stdcall
#endif
using HRESULT=std::int32_t;
using HWND=void*;
using LPGUID=GUID*;
using REFGUID=const GUID&;
using REFIID=const GUID&;
#ifndef S_OK
#define S_OK ((HRESULT)0)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif
#ifndef E_POINTER
#define E_POINTER ((HRESULT)0x80004003L)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr) ((HRESULT)(hr) < 0)
#endif
struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
protected:
    ~IUnknown() = default;
};
using LPUNKNOWN=IUnknown*;
