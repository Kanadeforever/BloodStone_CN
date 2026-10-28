#pragma once
// 仅供无 Windows SDK 的语法检查使用。
// 本文件模拟来自静态链接 SDL3 的单个类型名称查询声明；正式构建使用官方 SDL3/SDL.h。
enum SDL_GamepadType : int;
extern "C" const char* SDL_GetGamepadStringForType(SDL_GamepadType);
