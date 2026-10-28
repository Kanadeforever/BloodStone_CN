#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>

namespace
{
constexpr std::size_t kStyleCount = 3;
constexpr std::size_t kIconCount = 24;

struct FakeTexture
{
    int id = 0;
    bool released = false;
};

struct Candidate
{
    FakeTexture original{1, false};
    std::array<FakeTexture*, kStyleCount> global{};
    std::array<std::array<FakeTexture*, kIconCount>, kStyleCount> event{};
    bool confirmed = true;
};

struct Runtime
{
    int style = 0;
    bool controllerMode = true;
    FakeTexture* bound = nullptr;
    int nextId = 100;
};

FakeTexture* EnsureGlobal(Runtime& runtime, Candidate& candidate, int style)
{
    FakeTexture*& cached = candidate.global[static_cast<std::size_t>(style)];
    if (!cached)
        cached = new FakeTexture{runtime.nextId++, false};
    return cached;
}

bool IsKnownGlobal(const Candidate& candidate, const FakeTexture* texture)
{
    for (const FakeTexture* cached : candidate.global)
    {
        if (cached && cached == texture)
            return true;
    }
    return false;
}

bool IsKnownEvent(const Candidate& candidate, const FakeTexture* texture)
{
    for (const auto& style : candidate.event)
    {
        for (const FakeTexture* cached : style)
        {
            if (cached && cached == texture)
                return true;
        }
    }
    return false;
}

// 只建模 v1.4.15 的关键行为：family 是否变化都执行绑定检查；原图集或错误 family
// 的全局图集会被修复，事件专用图集保持不动。
bool ApplyStyle(Runtime& runtime, Candidate& candidate, int desiredStyle)
{
    const bool changed = runtime.style != desiredStyle;
    runtime.style = desiredStyle;

    if (!runtime.controllerMode || !candidate.confirmed)
        return changed;

    FakeTexture* desired = candidate.global[static_cast<std::size_t>(desiredStyle)];
    const bool currentIsOriginal = runtime.bound == &candidate.original;
    const bool currentIsKnownGlobal = IsKnownGlobal(candidate, runtime.bound);
    const bool wrongGlobalStyle = currentIsKnownGlobal && runtime.bound != desired;

    // 事件纹理既不是 original，也不是 global，因此不会被低频全局自愈覆盖。
    if (!currentIsOriginal && !wrongGlobalStyle)
        return changed;

    if (!desired)
        desired = EnsureGlobal(runtime, candidate, desiredStyle);
    runtime.bound = desired;
    return true;
}

void ReleaseAll(Candidate& candidate)
{
    for (FakeTexture*& texture : candidate.global)
    {
        if (texture)
        {
            texture->released = true;
            delete texture;
            texture = nullptr;
        }
    }
    for (auto& style : candidate.event)
    {
        for (FakeTexture*& texture : style)
        {
            if (texture)
            {
                texture->released = true;
                delete texture;
                texture = nullptr;
            }
        }
    }
}
} // namespace

int main()
{
    Candidate candidate;
    Runtime runtime;

    // 回归核心：Xbox -> Xbox，family 没变化，但状态块恢复原键盘图集时仍必须自愈。
    runtime.style = 0;
    runtime.bound = &candidate.original;
    assert(ApplyStyle(runtime, candidate, 0));
    FakeTexture* xbox = candidate.global[0];
    assert(xbox != nullptr && runtime.bound == xbox);

    // 切换到 PlayStation：旧 Xbox 缓存不能释放，新缓存创建后立即重绑。
    assert(ApplyStyle(runtime, candidate, 1));
    FakeTexture* playStation = candidate.global[1];
    assert(playStation != nullptr && runtime.bound == playStation);
    assert(candidate.global[0] == xbox && !xbox->released);

    // 错误 family 的旧全局图集被状态块恢复时，应恢复为当前 PlayStation 图集。
    runtime.bound = xbox;
    assert(ApplyStyle(runtime, candidate, 1));
    assert(runtime.bound == playStation);

    // 事件专用纹理不能被 750 ms 的全局自愈强行覆盖。
    candidate.event[1][6] = new FakeTexture{500, false};
    FakeTexture* eventTexture = candidate.event[1][6];
    runtime.bound = eventTexture;
    const bool eventResult = ApplyStyle(runtime, candidate, 1);
    assert(!eventResult);
    assert(runtime.bound == eventTexture);
    assert(IsKnownEvent(candidate, runtime.bound));

    // 键鼠模式保留原图集，不执行手柄图集修复。
    runtime.controllerMode = false;
    runtime.bound = &candidate.original;
    assert(!ApplyStyle(runtime, candidate, 1));
    assert(runtime.bound == &candidate.original);

    // Reset/候选销毁时统一释放所有 style 缓存。
    ReleaseAll(candidate);
    for (FakeTexture* texture : candidate.global)
        assert(texture == nullptr);
    for (const auto& style : candidate.event)
        for (FakeTexture* texture : style)
            assert(texture == nullptr);

    std::cout << "auto atlas cache regression harness passed\n";
    return 0;
}
