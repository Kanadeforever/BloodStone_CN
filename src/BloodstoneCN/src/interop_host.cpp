#include "interop_host.h"
#include "log.h"

#include <array>

namespace bscn::interop {
namespace {
constexpr size_t kMaxClients = 4;
constexpr size_t kRecentTextSlots = 64;
constexpr DWORD kMaximumRecentWindowMs = 2000u;

struct ClientSlot {
    bool used = false;
    BSCND3D9ClientV1 callbacks{};
    IDirect3DDevice9* attached_device = nullptr; // game-owned raw pointer
};

struct ClientSnapshot {
    std::array<BSCND3D9ClientV1, kMaxClients> clients{};
    size_t count = 0;
};

struct RecentTextSlot {
    std::atomic<uint64_t> hash{0};
    std::atomic<DWORD> tick{0};
    std::atomic<uint32_t> units{0};
};

SRWLOCK g_client_lock = SRWLOCK_INIT;
std::array<ClientSlot, kMaxClients> g_clients{};
std::atomic<unsigned> g_client_count{0};
std::atomic<bool> g_pending_attach{false};
std::atomic<bool> g_host_ready{false};
std::atomic<IDirect3DDevice9*> g_current_device{nullptr};
std::array<RecentTextSlot, kRecentTextSlots> g_recent_text{};
std::atomic<uint32_t> g_recent_write{0};

wchar_t LowerAscii(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool IsWhitespace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
}

// Hash the user-visible body. Speaker prefixes such as "Nicole: ..." are
// ignored so NPC's body-only LOC text can match the game's native subtitle.
// Whitespace is collapsed and common markup tokens are ignored. No allocation
// is performed on the hot SubmitText path.
uint64_t ComparableTextHash(const wchar_t* text, uint32_t* comparable_units) {
    if (comparable_units) *comparable_units = 0;
    if (!text || !*text) return 0;

    size_t length = 0;
    while (text[length] && length < 16384u) ++length;
    if (!length || length >= 16384u) return 0;

    size_t begin = 0;
    while (begin < length && IsWhitespace(text[begin])) ++begin;

    // Detect a short speaker prefix. Avoid scanning arbitrarily far into a
    // sentence so normal punctuation does not become a false prefix split.
    size_t colon = SIZE_MAX;
    const size_t prefix_limit = std::min(length, begin + 32u);
    for (size_t i = begin; i < prefix_limit; ++i) {
        if (text[i] == L'\n' || text[i] == L'\r') break;
        if (text[i] == L':' || text[i] == 0xFF1A) {
            if (i > begin && i - begin <= 24u) colon = i;
            break;
        }
    }
    if (colon != SIZE_MAX) {
        size_t body = colon + 1u;
        while (body < length && IsWhitespace(text[body])) ++body;
        if (body < length) begin = body;
    }

    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffset;
    uint32_t units = 0;
    bool pending_space = false;

    auto feed = [&](wchar_t value) {
        const uint16_t u = static_cast<uint16_t>(LowerAscii(value));
        hash ^= static_cast<uint8_t>(u & 0xFFu);
        hash *= kPrime;
        hash ^= static_cast<uint8_t>((u >> 8u) & 0xFFu);
        hash *= kPrime;
        ++units;
    };

    for (size_t i = begin; i < length;) {
        // ~~{TOKEN}~~
        if (i + 2u < length && text[i] == L'~' && text[i + 1u] == L'~' &&
            text[i + 2u] == L'{') {
            size_t close = i + 3u;
            while (close + 2u < length &&
                   !(text[close] == L'}' && text[close + 1u] == L'~' &&
                     text[close + 2u] == L'~')) {
                ++close;
            }
            if (close + 2u < length) {
                i = close + 3u;
                continue;
            }
        }
        // {TOKEN}
        if (text[i] == L'{') {
            size_t close = i + 1u;
            while (close < length && close - i <= 64u && text[close] != L'}')
                ++close;
            if (close < length && text[close] == L'}') {
                i = close + 1u;
                continue;
            }
        }
        // ~style~
        if (text[i] == L'~') {
            size_t close = i + 1u;
            while (close < length && close - i <= 24u && text[close] != L'~')
                ++close;
            if (close < length && text[close] == L'~') {
                i = close + 1u;
                continue;
            }
        }

        const wchar_t c = text[i++];
        if (IsWhitespace(c)) {
            if (units) pending_space = true;
            continue;
        }
        if (pending_space) {
            feed(L' ');
            pending_space = false;
        }
        feed(c);
    }

    if (comparable_units) *comparable_units = units;
    return units ? hash : 0;
}

ClientSnapshot TakeSnapshot() {
    ClientSnapshot result;
    if (!g_client_count.load(std::memory_order_acquire)) return result;
    AcquireSRWLockShared(&g_client_lock);
    for (const auto& slot : g_clients) {
        if (!slot.used) continue;
        result.clients[result.count++] = slot.callbacks;
    }
    ReleaseSRWLockShared(&g_client_lock);
    return result;
}

void AttachPendingClients(IDirect3DDevice9* device) {
    if (!device || !g_pending_attach.exchange(false, std::memory_order_acq_rel))
        return;

    ClientSnapshot pending;
    bool still_pending = false;
    AcquireSRWLockExclusive(&g_client_lock);
    for (auto& slot : g_clients) {
        if (!slot.used || slot.attached_device == device) continue;
        slot.attached_device = device;
        pending.clients[pending.count++] = slot.callbacks;
    }
    for (const auto& slot : g_clients) {
        if (slot.used && slot.attached_device != device) {
            still_pending = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_client_lock);
    if (still_pending) g_pending_attach.store(true, std::memory_order_release);

    // Important: called only from Bond's D3D thread (CreateDevice/Reset/EndScene),
    // never from the worker thread that registered the client.
    for (size_t i = 0; i < pending.count; ++i) {
        const auto& cb = pending.clients[i];
        if (cb.on_device_created) cb.on_device_created(cb.context, device);
    }
}
} // namespace

void SetHostReady(bool ready) {
    g_host_ready.store(ready, std::memory_order_release);
}

void NotifyDeviceCreated(IDirect3DDevice9* device) {
    if (!device) return;
    g_current_device.store(device, std::memory_order_release);
    if (g_client_count.load(std::memory_order_acquire)) {
        g_pending_attach.store(true, std::memory_order_release);
        AttachPendingClients(device);
    }
}

void NotifyBeforeReset(IDirect3DDevice9* device,
                       const D3DPRESENT_PARAMETERS* params) {
    if (device && g_client_count.load(std::memory_order_acquire)) {
        g_pending_attach.store(true, std::memory_order_release);
        AttachPendingClients(device);
    }
    const auto snapshot = TakeSnapshot();
    for (size_t i = 0; i < snapshot.count; ++i) {
        const auto& cb = snapshot.clients[i];
        if (cb.on_before_reset) cb.on_before_reset(cb.context, device, params);
    }
}

void NotifyAfterReset(IDirect3DDevice9* device, HRESULT result) {
    if (SUCCEEDED(result) && device)
        g_current_device.store(device, std::memory_order_release);
    const auto snapshot = TakeSnapshot();
    for (size_t i = 0; i < snapshot.count; ++i) {
        const auto& cb = snapshot.clients[i];
        if (cb.on_after_reset) cb.on_after_reset(cb.context, device, result);
    }
}

void NotifyEndScene(IDirect3DDevice9* device) {
    if (device) g_current_device.store(device, std::memory_order_release);
    if (!g_client_count.load(std::memory_order_acquire)) return;
    if (g_pending_attach.load(std::memory_order_acquire))
        AttachPendingClients(device);
    const auto snapshot = TakeSnapshot();
    for (size_t i = 0; i < snapshot.count; ++i) {
        const auto& cb = snapshot.clients[i];
        if (cb.on_end_scene) cb.on_end_scene(cb.context, device);
    }
}

void ObserveSubmittedText(const wchar_t* text) {
    uint32_t units = 0;
    const uint64_t hash = ComparableTextHash(text, &units);
    // Very short strings are too collision-prone semantically ("走", "好" etc.)
    // for duplicate suppression. They remain fully handled by localization.
    if (!hash || units < 3u) return;
    const uint32_t index = g_recent_write.fetch_add(1, std::memory_order_relaxed) %
                           static_cast<uint32_t>(kRecentTextSlots);
    auto& slot = g_recent_text[index];
    slot.hash.store(hash, std::memory_order_relaxed);
    slot.units.store(units, std::memory_order_relaxed);
    slot.tick.store(GetTickCount(), std::memory_order_release);
}

void Shutdown() {
    SetHostReady(false);
    g_current_device.store(nullptr, std::memory_order_release);
    AcquireSRWLockExclusive(&g_client_lock);
    for (auto& slot : g_clients) slot = ClientSlot{};
    ReleaseSRWLockExclusive(&g_client_lock);
    g_client_count.store(0, std::memory_order_release);
    g_pending_attach.store(false, std::memory_order_release);
}

bool HostReadyForExport() {
    return g_host_ready.load(std::memory_order_acquire);
}

bool RegisterClientForExport(const BSCND3D9ClientV1* client) {
    if (!client || client->size != sizeof(BSCND3D9ClientV1) ||
        client->abi_version != BSCN_D3D9_INTEROP_ABI_V1 || !client->context ||
        !HostReadyForExport()) {
        return false;
    }
    bool registered = false;
    bool was_new = false;
    AcquireSRWLockExclusive(&g_client_lock);
    for (auto& slot : g_clients) {
        if (!slot.used || slot.callbacks.context != client->context) continue;
        slot.callbacks = *client;
        slot.attached_device = nullptr;
        registered = true;
        break;
    }
    if (!registered) {
        for (auto& slot : g_clients) {
            if (slot.used) continue;
            slot.used = true;
            slot.callbacks = *client;
            slot.attached_device = nullptr;
            registered = true;
            was_new = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_client_lock);
    if (!registered) return false;
    if (was_new) g_client_count.fetch_add(1, std::memory_order_acq_rel);
    g_pending_attach.store(true, std::memory_order_release);
    return true;
}

void UnregisterClientForExport(void* context) {
    if (!context) return;
    bool removed = false;
    AcquireSRWLockExclusive(&g_client_lock);
    for (auto& slot : g_clients) {
        if (!slot.used || slot.callbacks.context != context) continue;
        slot = ClientSlot{};
        removed = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_client_lock);
    if (removed) g_client_count.fetch_sub(1, std::memory_order_acq_rel);
}

bool WasRecentlySubmittedForExport(const wchar_t* text, DWORD window_ms) {
    uint32_t units = 0;
    const uint64_t hash = ComparableTextHash(text, &units);
    if (!hash || units < 3u) return false;
    const DWORD window = std::clamp<DWORD>(window_ms, 1u, kMaximumRecentWindowMs);
    const DWORD now = GetTickCount();
    for (const auto& slot : g_recent_text) {
        const DWORD tick = slot.tick.load(std::memory_order_acquire);
        if (!tick || now - tick > window) continue;
        if (slot.units.load(std::memory_order_relaxed) != units) continue;
        if (slot.hash.load(std::memory_order_relaxed) == hash) return true;
    }
    return false;
}

unsigned ClientCountForExport() {
    return g_client_count.load(std::memory_order_acquire);
}

} // namespace bscn::interop

extern "C" __declspec(dllexport) uint32_t WINAPI
BSCN_D3D9_GetInteropAbiVersionV1() {
    return BSCN_D3D9_INTEROP_ABI_V1;
}

extern "C" __declspec(dllexport) BOOL WINAPI BSCN_D3D9_IsHostReadyV1() {
    return bscn::interop::HostReadyForExport() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI BSCN_D3D9_RegisterClientV1(
    const BSCND3D9ClientV1* client) {
    if (!bscn::interop::RegisterClientForExport(client)) {
        BSCN_LOG_ERROR("D3D9_INTEROP_CLIENT_REGISTER_FAILED abi=0x%08X",
                       client ? client->abi_version : 0u);
        return FALSE;
    }
    BSCN_LOG_INFO("D3D9_INTEROP_CLIENT_REGISTERED abi=0x%08X clients=%u",
                  BSCN_D3D9_INTEROP_ABI_V1,
                  bscn::interop::ClientCountForExport());
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI BSCN_D3D9_UnregisterClientV1(
    void* context) {
    bscn::interop::UnregisterClientForExport(context);
}

extern "C" __declspec(dllexport) BOOL WINAPI BSCN_Text_WasRecentlySubmittedV1(
    const wchar_t* text, DWORD window_ms) {
    return bscn::interop::WasRecentlySubmittedForExport(text, window_ms)
               ? TRUE
               : FALSE;
}
