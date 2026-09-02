#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <cstdint>

// BloodstoneCN <-> NPCSubtitles 独立 ASI 互操作 ABI v1。
// 该共享头只定义 POD callback 表；不声明 dllexport，避免 NPC 客户端
// 被 MSVC 链接器误要求导出/解析 Host 符号。Host 导出由
// BloodstoneCN 的 interop_host.cpp 单独定义，客户端使用 GetProcAddress。
#define BSCN_D3D9_INTEROP_ABI_V1 0x00010000u

struct BSCND3D9ClientV1 {
    uint32_t size;
    uint32_t abi_version;
    void* context;
    void (WINAPI* on_device_created)(void* context, IDirect3DDevice9* device);
    void (WINAPI* on_before_reset)(void* context, IDirect3DDevice9* device,
                                   const D3DPRESENT_PARAMETERS* params);
    void (WINAPI* on_after_reset)(void* context, IDirect3DDevice9* device,
                                  HRESULT result);
    void (WINAPI* on_end_scene)(void* context, IDirect3DDevice9* device);
};
