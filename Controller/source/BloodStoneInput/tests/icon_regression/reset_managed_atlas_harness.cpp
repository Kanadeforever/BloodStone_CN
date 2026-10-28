#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
constexpr int D3DPOOL_DEFAULT = 0;
constexpr int D3DPOOL_MANAGED = 1;

struct Candidate {
    int pool = D3DPOOL_DEFAULT;
    bool atlasConfirmed = false;
    bool hasSourcePixels = false;
    int replacementCount = 0;
    int inspectionAttempts = 0;
    uint32_t lastPixelCrc32 = 0;
    uint32_t firstObservedTick = 0;
    uint32_t lastInspectionTick = 0;
    bool hasFirstObservedTick = false;
    bool permanentlyRejected = false;
    bool hasObservedStage = false;
    uint32_t lastObservedStage = 0;
};

void ResetUnconfirmedManagedCandidateInspection(Candidate& c) {
    if (c.atlasConfirmed || c.pool != D3DPOOL_MANAGED)
        return;
    c.inspectionAttempts = 0;
    c.lastPixelCrc32 = 0;
    c.firstObservedTick = 0;
    c.lastInspectionTick = 0;
    c.hasFirstObservedTick = false;
    c.permanentlyRejected = false;
}

void CommitSuccessfulReset(std::vector<Candidate>& candidates) {
    for (auto it = candidates.begin(); it != candidates.end();) {
        if (it->pool == D3DPOOL_MANAGED) {
            ResetUnconfirmedManagedCandidateInspection(*it);
            ++it;
            continue;
        }
        it = candidates.erase(it);
    }
}

int ConfirmedCount(const std::vector<Candidate>& candidates) {
    int n = 0;
    for (const auto& c : candidates)
        n += c.atlasConfirmed ? 1 : 0;
    return n;
}
}

int main() {
    Candidate confirmedManaged;
    confirmedManaged.pool = D3DPOOL_MANAGED;
    confirmedManaged.atlasConfirmed = true;
    confirmedManaged.hasSourcePixels = true;
    confirmedManaged.replacementCount = 3;
    confirmedManaged.inspectionAttempts = 7;
    confirmedManaged.lastPixelCrc32 = 0x29F36B36u;
    confirmedManaged.hasObservedStage = true;
    confirmedManaged.lastObservedStage = 0;

    Candidate unconfirmedManaged;
    unconfirmedManaged.pool = D3DPOOL_MANAGED;
    unconfirmedManaged.atlasConfirmed = false;
    unconfirmedManaged.inspectionAttempts = 48;
    unconfirmedManaged.lastPixelCrc32 = 0x12345678u;
    unconfirmedManaged.firstObservedTick = 100;
    unconfirmedManaged.lastInspectionTick = 5000;
    unconfirmedManaged.hasFirstObservedTick = true;
    unconfirmedManaged.permanentlyRejected = true;

    Candidate defaultPool;
    defaultPool.pool = D3DPOOL_DEFAULT;
    defaultPool.atlasConfirmed = false;
    defaultPool.hasSourcePixels = true;
    defaultPool.replacementCount = 2;

    std::vector<Candidate> before = {confirmedManaged, unconfirmedManaged, defaultPool};

    // Reset 失败：Hook 不提交任何清理，整个状态必须原样保留。
    std::vector<Candidate> failed = before;
    assert(failed.size() == 3);
    assert(failed[0].atlasConfirmed && failed[0].replacementCount == 3);
    assert(failed[1].permanentlyRejected && failed[1].inspectionAttempts == 48);
    assert(failed[2].pool == D3DPOOL_DEFAULT);

    // Reset 成功：managed 图集必须保留；非 managed 候选才被清理。
    std::vector<Candidate> succeeded = before;
    CommitSuccessfulReset(succeeded);
    assert(succeeded.size() == 2);
    assert(succeeded[0].pool == D3DPOOL_MANAGED);
    assert(succeeded[0].atlasConfirmed);
    assert(succeeded[0].hasSourcePixels);
    assert(succeeded[0].replacementCount == 3);
    assert(succeeded[0].lastPixelCrc32 == 0x29F36B36u);
    assert(succeeded[0].hasObservedStage && succeeded[0].lastObservedStage == 0);

    // 未确认 managed 候选获得新的观察窗口，但资源本身仍保留。
    assert(succeeded[1].pool == D3DPOOL_MANAGED);
    assert(!succeeded[1].atlasConfirmed);
    assert(succeeded[1].inspectionAttempts == 0);
    assert(succeeded[1].lastPixelCrc32 == 0);
    assert(!succeeded[1].hasFirstObservedTick);
    assert(!succeeded[1].permanentlyRejected);

    assert(ConfirmedCount(succeeded) == 1);
    std::cout << "reset-managed-atlas harness PASS\n";
    return 0;
}
