#pragma once
#include <windows.h>

#define TH32CS_SNAPMODULE 0x00000008u
#define TH32CS_SNAPMODULE32 0x00000010u

struct MODULEENTRY32W {
    DWORD dwSize;
    DWORD th32ModuleID;
    DWORD th32ProcessID;
    DWORD GlblcntUsage;
    DWORD ProccntUsage;
    BYTE* modBaseAddr;
    DWORD modBaseSize;
    HMODULE hModule;
    wchar_t szModule[256];
    wchar_t szExePath[MAX_PATH];
};

extern "C" {
HANDLE CreateToolhelp32Snapshot(DWORD,DWORD);
BOOL Module32FirstW(HANDLE,MODULEENTRY32W*);
BOOL Module32NextW(HANDLE,MODULEENTRY32W*);
DWORD GetCurrentProcessId();
}
