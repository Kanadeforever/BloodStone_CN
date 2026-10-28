#include <cassert>
#include <cstdint>
#include <iostream>

using DWORD = std::uint32_t;

constexpr DWORD kInitialGraceMs = 250;
constexpr DWORD kIntervalMs = 100;
constexpr DWORD kTimeoutMs = 5000;
constexpr int kMaximumAttempts = 48;

struct CandidateState {
    int inspectionAttempts = 0;
    DWORD firstObservedTick = 0;
    DWORD lastInspectionTick = 0;
    bool hasFirstObservedTick = false;
    bool atlasConfirmed = false;
    bool permanentlyRejected = false;
};

constexpr bool elapsed(DWORD now, DWORD earlier, DWORD interval) {
    return static_cast<DWORD>(now - earlier) >= interval;
}

constexpr bool expired(const CandidateState& candidate, DWORD now) {
    return candidate.hasFirstObservedTick &&
           elapsed(now, candidate.firstObservedTick, kTimeoutMs);
}

bool shouldInspect(CandidateState& candidate, DWORD now) {
    if (candidate.atlasConfirmed || candidate.permanentlyRejected)
        return false;
    if (!candidate.hasFirstObservedTick) {
        candidate.firstObservedTick = now;
        candidate.hasFirstObservedTick = true;
        return false;
    }
    if (expired(candidate, now) || candidate.inspectionAttempts >= kMaximumAttempts) {
        candidate.permanentlyRejected = true;
        return false;
    }
    if (candidate.inspectionAttempts == 0)
        return elapsed(now, candidate.firstObservedTick, kInitialGraceMs);
    return elapsed(now, candidate.lastInspectionTick, kIntervalMs);
}

void markInspection(CandidateState& candidate, DWORD now) {
    candidate.lastInspectionTick = now;
    ++candidate.inspectionAttempts;
}

CandidateState simulate(bool loggingEnabled) {
    CandidateState candidate{};
    const DWORD calls[] = {1000, 1001, 1002, 1100, 1249, 1250, 1251, 1349, 1350, 1450};
    for (DWORD now : calls) {
        // 模拟日志启用时存在额外工作，但不允许它改变功能时钟或状态决策。
        volatile int logWork = 0;
        if (loggingEnabled) {
            for (int i = 0; i < 100; ++i)
                logWork += i;
        }
        (void)logWork;
        if (shouldInspect(candidate, now))
            markInspection(candidate, now);
    }
    return candidate;
}

int main() {
    CandidateState candidate{};
    assert(!shouldInspect(candidate, 1000));       // 首次观察只启动宽限期。
    assert(!shouldInspect(candidate, 1001));       // 同一帧快速调用不得消耗检查次数。
    assert(!shouldInspect(candidate, 1249));
    assert(shouldInspect(candidate, 1250));
    markInspection(candidate, 1250);
    assert(!shouldInspect(candidate, 1349));
    assert(shouldInspect(candidate, 1350));
    markInspection(candidate, 1350);

    // 相同像素不参与调度，也不能触发立即永久排除；调度只看单调时钟。
    assert(!candidate.permanentlyRejected);
    assert(!shouldInspect(candidate, 1399));
    assert(shouldInspect(candidate, 1450));

    // GetTickCount 回绕必须安全。
    CandidateState wrapped{};
    assert(!shouldInspect(wrapped, 0xFFFFFF80u));
    assert(!shouldInspect(wrapped, 0x00000079u));
    assert(shouldInspect(wrapped, 0x0000007Au));

    // 日志开关不得改变相同调用时间线下的检查状态。
    const CandidateState logOff = simulate(false);
    const CandidateState logOn = simulate(true);
    assert(logOff.inspectionAttempts == logOn.inspectionAttempts);
    assert(logOff.firstObservedTick == logOn.firstObservedTick);
    assert(logOff.lastInspectionTick == logOn.lastInspectionTick);
    assert(logOff.permanentlyRejected == logOn.permanentlyRejected);
    assert(logOff.inspectionAttempts == 3);

    // 超时后安全退休。
    CandidateState timeout{};
    assert(!shouldInspect(timeout, 2000));
    assert(!shouldInspect(timeout, 7000));
    assert(timeout.permanentlyRejected);

    std::cout << "atlas scan timing harness passed: grace=250 interval=100 timeout=5000 "
                 "max=48 log_independent=1 wrap_safe=1\n";
}
