/*
 * 使用 SDL3 的实际静态库执行最基础的控制器检查。
 * 这个测试程序用于验证 SDL3 可以脱离 SDL3 动态库被静态链接，
 * 并能初始化 Gamepad 子系统；它不检查 JoyShim 的 Windows 代理 DLL。
 */
#include <stdio.h>
#include <SDL3/SDL.h>

int main(void) {
    /* 先确认真正链接的是预定的 3.4.18，避免系统其他版本混入。 */
    const int version = SDL_GetVersion();
    printf("SDL3=%d.%d.%d\n", SDL_VERSIONNUM_MAJOR(version),
           SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version));
    if (version != SDL_VERSIONNUM(3, 4, 18)) {
        return 2;
    }

    /* 初始化控制器子系统。即使电脑上没有手柄，初始化也应该成功。 */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "Gamepad 初始化失败：%s\n", SDL_GetError());
        return 3;
    }

    /* 枚举后必须释放 SDL 返回的内存，并成对退出已初始化的子系统。 */
    int count = 0;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&count);
    printf("Gamepad 初始化成功，当前设备数=%d\n", count);
    SDL_free(gamepads);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return 0;
}
