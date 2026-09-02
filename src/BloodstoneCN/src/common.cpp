#include "common.h"

namespace bscn {

uintptr_t RuntimeAddress(HMODULE module, uint32_t static_va) {
    return reinterpret_cast<uintptr_t>(module) +
           (static_va - kPreferredImageBase);
}

bool ReadBytes(const void* address, void* output, size_t size) {
    if (!address || !output || size == 0) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, size, &read) != FALSE &&
           read == size;
}

bool BytesEqual(const void* address, const void* expected, size_t size) {
    if (!address || !expected || size == 0) return false;
    std::vector<uint8_t> observed(size);
    return ReadBytes(address, observed.data(), size) &&
           memcmp(observed.data(), expected, size) == 0;
}

bool WriteCode(void* address, const void* bytes, size_t size) {
    if (!address || !bytes || size == 0) return false;
    DWORD old_protect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    memcpy(address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

bool PatchPointer(void** slot, void* expected, void* replacement) {
    if (!slot || !expected || !replacement) return false;
    void* observed = nullptr;
    if (!ReadBytes(slot, &observed, sizeof(observed)) || observed != expected) return false;
    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot), replacement, expected);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return observed == expected;
}

bool PatchPointerChain(void** slot, void* replacement, void** previous) {
    if (!slot || !replacement || !previous) return false;
    for (unsigned attempt = 0; attempt < 16u; ++attempt) {
        void* observed = nullptr;
        if (!ReadBytes(slot, &observed, sizeof(observed)) || !observed ||
            observed == replacement) {
            return false;
        }
        if (PatchPointer(slot, observed, replacement)) {
            *previous = observed;
            return true;
        }
        SwitchToThread();
    }
    return false;
}

void RestorePointer(void** slot, void* replacement, void* original) {
    if (!slot || !replacement || !original) return;
    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) return;
    InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),
                                      original, replacement);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
}

bool PatchRelativeCall(uint8_t* instruction, void* replacement,
                       void** original_target, int32_t* original_disp) {
    if (!instruction || !replacement || !original_target || !original_disp) return false;
    uint8_t bytes[5]{};
    if (!ReadBytes(instruction, bytes, sizeof(bytes)) || bytes[0] != 0xE8) return false;
    int32_t old_disp = 0;
    memcpy(&old_disp, bytes + 1, sizeof(old_disp));
    *original_target = instruction + 5 + old_disp;
    *original_disp = old_disp;
    const intptr_t delta = reinterpret_cast<uint8_t*>(replacement) - (instruction + 5);
    const int32_t new_disp = static_cast<int32_t>(delta);
    uint8_t patched[5] = {0xE8, 0, 0, 0, 0};
    memcpy(patched + 1, &new_disp, sizeof(new_disp));
    return WriteCode(instruction, patched, sizeof(patched));
}

void RestoreRelativeCall(uint8_t* instruction, void* replacement,
                         int32_t original_disp) {
    if (!instruction || !replacement) return;
    uint8_t current[5]{};
    if (!ReadBytes(instruction, current, sizeof(current)) || current[0] != 0xE8) return;
    int32_t current_disp = 0;
    memcpy(&current_disp, current + 1, sizeof(current_disp));
    if (instruction + 5 + current_disp != replacement) return;
    uint8_t restored[5] = {0xE8, 0, 0, 0, 0};
    memcpy(restored + 1, &original_disp, sizeof(original_disp));
    WriteCode(instruction, restored, sizeof(restored));
}

bool ValidateBondModule(HMODULE module) {
    if (!module) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    IMAGE_DOS_HEADER dos{};
    if (!ReadBytes(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
        return false;
    IMAGE_NT_HEADERS32 nt{};
    if (!ReadBytes(base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    return nt.FileHeader.TimeDateStamp == kKnownTimestamp &&
           nt.OptionalHeader.SizeOfImage == kKnownImageSize;
}

bool PatchImport(HMODULE module, const char* imported_module,
                 const char* imported_name, void* replacement,
                 void** original) {
    if (!module || !imported_module || !imported_name || !replacement || !original) return false;
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dll, imported_module) != 0) continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->FirstThunk);
        auto* name_thunk = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->OriginalFirstThunk)
            : thunk;
        for (; name_thunk->u1.AddressOfData; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(name_thunk->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by_name->Name), imported_name) != 0) continue;
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            // 多个独立 ASI 可能并发挂同一个 D3D9 IAT。始终链到安装瞬间
            // 的真实当前值，禁止无条件覆盖其它 ASI 的 Hook。
            return PatchPointerChain(slot, replacement, original);
        }
    }
    return false;
}

std::wstring ModuleDirectory(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L".";
    std::wstring value(path, n);
    const size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (right.size() >= 2 && right[1] == L':') return right;
    if (right[0] == L'\\' || right[0] == L'/') return right;
    std::wstring out = left;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out += right;
    return out;
}

namespace {
bool IsAsciiAlphaUnit(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}
bool IsAsciiUpperUnit(wchar_t c) { return c >= L'A' && c <= L'Z'; }
bool IsAsciiDigitUnit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool IsIdentityTokenChar(wchar_t c) {
    return IsAsciiAlphaUnit(c) || IsAsciiDigitUnit(c) || c == L'.' ||
           c == L'_' || c == L'-' || c == L'/';
}
wchar_t LowerAsciiUnit(wchar_t c) {
    return (c >= L'A' && c <= L'Z')
               ? static_cast<wchar_t>(c - L'A' + L'a')
               : c;
}
bool IdentityTokenSpan(const wchar_t* text, size_t units) {
    if (!text || units < 2u || units > 16u) return false;
    bool alpha = false;
    bool digit = false;
    bool punctuation = false;
    bool all_alpha_upper = true;
    size_t alpha_count = 0u;
    for (size_t i = 0; i < units; ++i) {
        const wchar_t c = text[i];
        if (IsAsciiAlphaUnit(c)) {
            alpha = true;
            ++alpha_count;
            if (!IsAsciiUpperUnit(c)) all_alpha_upper = false;
        } else if (IsAsciiDigitUnit(c)) {
            digit = true;
        } else if (c == L'.' || c == L'_' || c == L'-' || c == L'/') {
            punctuation = true;
        } else {
            return false;
        }
    }
    if (alpha && digit) return true;
    return alpha_count >= 2u && punctuation && all_alpha_upper;
}
bool ContainsAsciiNoCaseSpan(const wchar_t* text, size_t text_units,
                             const wchar_t* needle, size_t needle_units) {
    if (!text || !needle || needle_units == 0u || needle_units > text_units)
        return false;
    for (size_t i = 0; i + needle_units <= text_units; ++i) {
        bool same = true;
        for (size_t j = 0; j < needle_units; ++j) {
            if (LowerAsciiUnit(text[i + j]) != LowerAsciiUnit(needle[j])) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return false;
}
bool IsPureIdentityLabel(const wchar_t* text, size_t units) {
    if (!text || units == 0u) return false;
    size_t begin = 0u;
    size_t end = units;
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) ++begin;
    while (end > begin && (text[end - 1u] == L' ' || text[end - 1u] == L'\t')) --end;
    return begin < end && IdentityTokenSpan(text + begin, end - begin);
}
}  // namespace

bool IsDynamicRuntimeScalar(const wchar_t* text, size_t units) {
    if (!text || units == 0u || units > 64u) return false;
    size_t begin = 0u;
    size_t end = units;
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) ++begin;
    while (end > begin && (text[end - 1u] == L' ' || text[end - 1u] == L'\t')) --end;
    if (begin == end) return false;
    bool saw_digit = false;
    for (size_t i = begin; i < end; ++i) {
        const wchar_t c = text[i];
        if (IsAsciiDigitUnit(c)) {
            saw_digit = true;
            continue;
        }
        if (c == L' ' || c == L'\t' || c == L'+' || c == L'-' ||
            c == L'.' || c == L',' || c == L'/' || c == L':' ||
            c == L'%' || c == L'(' || c == L')' || c == L'[' ||
            c == L']' || c == L'|') {
            continue;
        }
        return false;
    }
    return saw_digit;
}

bool HasTechnicalIdentityToken(const wchar_t* text, size_t units) {
    if (!text || units == 0u) return false;
    for (size_t i = 0; i < units;) {
        if (!IsIdentityTokenChar(text[i])) {
            ++i;
            continue;
        }
        const size_t begin = i;
        while (i < units && IsIdentityTokenChar(text[i])) ++i;
        if (IdentityTokenSpan(text + begin, i - begin)) return true;
    }
    return false;
}

bool PreservesTechnicalIdentityTokens(const wchar_t* source,
                                      size_t source_units,
                                      const wchar_t* replacement,
                                      size_t replacement_units) {
    if (!source || !replacement) return true;
    for (size_t i = 0; i < source_units;) {
        if (!IsIdentityTokenChar(source[i])) {
            ++i;
            continue;
        }
        const size_t begin = i;
        while (i < source_units && IsIdentityTokenChar(source[i])) ++i;
        const size_t token_units = i - begin;
        if (!IdentityTokenSpan(source + begin, token_units)) continue;
        if (!ContainsAsciiNoCaseSpan(replacement, replacement_units,
                                     source + begin, token_units)) {
            return false;
        }
    }
    return true;
}

bool ShouldDeferTechnicalIdentityLabel(const wchar_t* source,
                                       size_t source_units,
                                       const wchar_t* replacement,
                                       size_t replacement_units) {
    if (!source || !replacement || source_units == 0u || replacement_units == 0u)
        return false;
    if (source_units == replacement_units &&
        std::wmemcmp(source, replacement, source_units) == 0) {
        return false;
    }
    if (!HasTechnicalIdentityToken(source, source_units)) return false;
    if (IsPureIdentityLabel(source, source_units)) return false;
    return PreservesTechnicalIdentityTokens(source, source_units,
                                            replacement, replacement_units);
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                            static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace bscn
