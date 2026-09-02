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
#include <initializer_list>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define BSCN_FASTCALL __fastcall
#define BSCN_CDECL __cdecl
#define BSCN_THISCALL __thiscall
#define BSCN_STDCALL __stdcall
#else
#define BSCN_FASTCALL __attribute__((fastcall))
#define BSCN_CDECL __attribute__((cdecl))
#define BSCN_THISCALL __attribute__((thiscall))
#define BSCN_STDCALL __attribute__((stdcall))
#endif

namespace bscn {

constexpr uint32_t kPreferredImageBase = 0x00400000u;
constexpr uint32_t kKnownTimestamp = 0x4CA36144u;
constexpr uint32_t kKnownImageSize = 0x010CC000u;

uintptr_t RuntimeAddress(HMODULE module, uint32_t static_va);
bool ReadBytes(const void* address, void* output, size_t size);
bool BytesEqual(const void* address, const void* expected, size_t size);
bool WriteCode(void* address, const void* bytes, size_t size);
bool PatchPointer(void** slot, void* expected, void* replacement);
bool PatchPointerChain(void** slot, void* replacement, void** previous);
void RestorePointer(void** slot, void* replacement, void* original);
bool PatchRelativeCall(uint8_t* instruction, void* replacement,
                       void** original_target, int32_t* original_disp);
void RestoreRelativeCall(uint8_t* instruction, void* replacement,
                         int32_t original_disp);
bool ValidateBondModule(HMODULE module);
bool PatchImport(HMODULE module, const char* imported_module,
                 const char* imported_name, void* replacement,
                 void** original);
std::wstring ModuleDirectory(HMODULE module);
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
std::string WideToUtf8(const std::wstring& text);

// W1 HUD 生命周期保护：只识别纯动态数值形态。包含任何 ASCII 字母/CJK
// 的字符串都不会被归类为动态标量，因此 P99/MP7/Ammo 10 等不会误命中。
bool IsDynamicRuntimeScalar(const wchar_t* text, size_t units);
inline bool IsDynamicRuntimeScalar(const std::wstring& text) {
    return IsDynamicRuntimeScalar(text.c_str(), text.size());
}

// 通用技术身份 token：字母+数字（P99/MP7/G36C）或带分隔符的短大写缩写
//（A.R. 等）。这里只描述字符串结构，不维护任何具体枪械/装备名单。
bool HasTechnicalIdentityToken(const wchar_t* text, size_t units);
inline bool HasTechnicalIdentityToken(const std::wstring& text) {
    return HasTechnicalIdentityToken(text.c_str(), text.size());
}
bool PreservesTechnicalIdentityTokens(const wchar_t* source, size_t source_units,
                                      const wchar_t* replacement,
                                      size_t replacement_units);
inline bool PreservesTechnicalIdentityTokens(const std::wstring& source,
                                             const std::wstring& replacement) {
    return PreservesTechnicalIdentityTokens(
        source.c_str(), source.size(), replacement.c_str(), replacement.size());
}
// 仅决定“GetText 是否保留游戏原指针、延迟到 SubmitText 再注入中文”。
// 该函数不改变候选选择，不拒绝中文：只有 source 含技术身份、且 replacement
// 保留同一身份时才返回 true；否则完全沿用 R10.4 原行为。
bool ShouldDeferTechnicalIdentityLabel(const wchar_t* source,
                                       size_t source_units,
                                       const wchar_t* replacement,
                                       size_t replacement_units);

}  // namespace bscn
