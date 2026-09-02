#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define NPC_FASTCALL __fastcall
#define NPC_CDECL __cdecl
#define NPC_THISCALL __thiscall
#define NPC_STDCALL __stdcall
#else
#define NPC_FASTCALL __attribute__((fastcall))
#define NPC_CDECL __attribute__((cdecl))
#define NPC_THISCALL __attribute__((thiscall))
#define NPC_STDCALL __attribute__((stdcall))
#endif

namespace npcsub {
constexpr uint32_t kPreferredImageBase = 0x00400000u;
constexpr uint32_t kKnownTimestamp = 0x4CA36144u;
constexpr uint32_t kKnownImageSize = 0x010CC000u;

uintptr_t RuntimeAddress(HMODULE module, uint32_t static_va);
bool ReadBytes(const void* address, void* output, size_t size);
bool WriteCode(void* address, const void* bytes, size_t size);
bool ValidateBondModule(HMODULE module);
bool PatchImportChain(HMODULE module, const char* imported_module,
                      const char* imported_name, void* replacement,
                      void** previous);
bool PatchPointerChain(void** slot, void* replacement, void** previous);
void RestorePointerIfOwned(void** slot, void* replacement, void* previous);
bool PatchRelativeCall(uint8_t* instruction, void* replacement,
                       void** previous_target, int32_t* previous_disp);
void RestoreRelativeCallIfOwned(uint8_t* instruction, void* replacement,
                                int32_t previous_disp);
std::wstring ModuleDirectory(HMODULE module);
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
std::string WideToUtf8(const std::wstring& text);
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
}
