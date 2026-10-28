#pragma once

namespace joyshim {

// 仅从 RuntimeBootstrapWorker 调用，不得在真实代理导出路径或 DllMain 同步执行。
// 初始化不以是否已连接手柄为条件：一旦代理入口被宿主实际调用，SDL3
// Gamepad 子系统始终加载并保持轮询，以便随后连接手柄时正常热插拔。
// 初始化期间发生的系统代理重入不会再次等待初始化，以避免 SDL 加载链死锁。
bool EnsureRuntimeInitialized() noexcept;

}  // namespace joyshim
