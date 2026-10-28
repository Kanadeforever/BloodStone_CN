#pragma once
#include <windows.h>

// 单 DLL 内部使用的稳定 C 接口；调用方只读取已经发布的手柄槽位快照。
// 将接口声明放在全局作用域，避免匿名命名空间引入内部链接符号。
#ifdef BSCN_SINGLE_DLL
extern "C" int WINAPI BondOneReadControllerInfo(
    unsigned int slot, int* type, char* typeName, unsigned int typeNameCapacity,
    char* name, unsigned int nameCapacity, unsigned short* vendor,
    unsigned int* deviceId);
extern "C" void BondIconsProcessAttach(HMODULE module);
extern "C" void BondIconsProcessDetach();
#endif
