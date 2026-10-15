// 最小化启动探针只验证 Win32 ASI 装载，不安装任何游戏或 D3D9 Hook。
// 与真正 NPC 功能版分别编译、独立安装，避免把“DLL 无法装载”与“运行时 Hook 崩溃”混为一谈。
#include "win32_minimal.h"

// 关闭 CRT 后，编译器对栈上固定数组清零仍然可能生成 memset 调用。
// 在诊断模块内提供简单实现，确保无法使用外部 C 运行时也能装载。
extern "C" void* CDECL memset(void* destination, int value, unsigned count) {
    U8* bytes = static_cast<U8*>(destination);
    for (unsigned i = 0; i < count; ++i) bytes[i] = U8(value);
    return destination;
}

namespace {
// 直接根据当前 DLL 模块取得安装目录，禁止假设 scripts 或 plugins。
// Windows 的 DllMain 处于加载器锁内，所以这里严格禁止创建线程、加载其它 DLL 或等待同步对象。
void SaveAttachLog(void* self) {
    wchar_t path[768] = {};
    U32 length = GetModuleFileNameW(self, path, 768);
    if (length < 5 || length >= 768) return;
    U32 begin = length;
    while (begin && path[begin-1] != L'\\' && path[begin-1] != L'/') --begin;
    if (!begin || begin + 24 >= 768) return;
    const wchar_t name[] = L"NPCSubtitles_boot.log";
    U32 index = 0;
    while (name[index]) { path[begin + index] = name[index]; ++index; }
    path[begin + index] = 0;
    // 只写一条固定长度消息，不访问堆、游戏代码、显卡设备或外部文件系统插件。
    Handle f = CreateFileW(path, 0x40000000u, 0x00000003u, nullptr, 2, 0x80u, nullptr);
    if (f == reinterpret_cast<Handle>(-1)) return;
    const char line[] = "NPCSubtitles v0.1a BOOT_ATTACH_OK\r\nNO_HOOKS_INSTALLED\r\n";
    U32 written = 0;
    WriteFile(f, line, U32(sizeof(line) - 1), &written, nullptr);
    CloseHandle(f);
}
}

extern "C" BOOL WINAPI DllMain(void* self, U32 reason, void*) {
    if (reason == 1) {
        OutputDebugStringA("NPCSubtitles v0.1a BOOT_ATTACH_BEGIN\r\n");
        SaveAttachLog(self);
    }
    return 1;
}
