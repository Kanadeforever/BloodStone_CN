#include "common.h"

namespace npcsub {
uintptr_t RuntimeAddress(HMODULE module, uint32_t static_va) {
    return reinterpret_cast<uintptr_t>(module) + (static_va - kPreferredImageBase);
}

bool ReadBytes(const void* address, void* output, size_t size) {
    if (!address || !output || !size) return false;
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, size, &done) != FALSE && done == size;
}

bool WriteCode(void* address, const void* bytes, size_t size) {
    if (!address || !bytes || !size) return false;
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

bool ValidateBondModule(HMODULE module) {
    if (!module) return false;
    auto* base = reinterpret_cast<const uint8_t*>(module);
    IMAGE_DOS_HEADER dos{};
    if (!ReadBytes(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
    IMAGE_NT_HEADERS32 nt{};
    if (!ReadBytes(base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
    return nt.FileHeader.TimeDateStamp == kKnownTimestamp &&
           nt.OptionalHeader.SizeOfImage == kKnownImageSize;
}

bool PatchImportChain(HMODULE module, const char* imported_module,
                      const char* imported_name, void* replacement,
                      void** previous) {
    if (!module || !imported_module || !imported_name || !replacement || !previous) return false;
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
            ? reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->OriginalFirstThunk) : thunk;
        for (; name_thunk->u1.AddressOfData; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(name_thunk->u1.Ordinal)) continue;
            auto* byname = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byname->Name), imported_name) != 0) continue;
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *previous = *slot;
            *slot = replacement;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

bool PatchPointerChain(void** slot, void* replacement, void** previous) {
    if (!slot || !replacement || !previous) return false;
    void* observed = nullptr;
    if (!ReadBytes(slot, &observed, sizeof(observed)) || !observed) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *previous = observed;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void RestorePointerIfOwned(void** slot, void* replacement, void* previous) {
    if (!slot || !replacement || !previous) return;
    void* observed = nullptr;
    if (!ReadBytes(slot, &observed, sizeof(observed)) || observed != replacement) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    *slot = previous;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
}

bool PatchRelativeCall(uint8_t* instruction, void* replacement,
                       void** previous_target, int32_t* previous_disp) {
    if (!instruction || !replacement || !previous_target || !previous_disp) return false;
    uint8_t bytes[5]{};
    if (!ReadBytes(instruction, bytes, sizeof(bytes)) || bytes[0] != 0xE8) return false;
    int32_t old_disp = 0;
    memcpy(&old_disp, bytes + 1, sizeof(old_disp));
    *previous_target = instruction + 5 + old_disp;
    *previous_disp = old_disp;
    const intptr_t delta = reinterpret_cast<uint8_t*>(replacement) - (instruction + 5);
    if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max()) return false;
    const int32_t new_disp = static_cast<int32_t>(delta);
    uint8_t patch[5] = {0xE8,0,0,0,0};
    memcpy(patch + 1, &new_disp, sizeof(new_disp));
    return WriteCode(instruction, patch, sizeof(patch));
}

void RestoreRelativeCallIfOwned(uint8_t* instruction, void* replacement,
                                int32_t previous_disp) {
    if (!instruction || !replacement) return;
    uint8_t bytes[5]{};
    if (!ReadBytes(instruction, bytes, sizeof(bytes)) || bytes[0] != 0xE8) return;
    int32_t current = 0;
    memcpy(&current, bytes + 1, sizeof(current));
    if (instruction + 5 + current != replacement) return;
    uint8_t patch[5] = {0xE8,0,0,0,0};
    memcpy(patch + 1, &previous_disp, sizeof(previous_disp));
    WriteCode(instruction, patch, sizeof(patch));
}

std::wstring ModuleDirectory(HMODULE module) {
    wchar_t path[1024]{};
    const DWORD n = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!n || n >= std::size(path)) return L".";
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

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirectoryExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
}
