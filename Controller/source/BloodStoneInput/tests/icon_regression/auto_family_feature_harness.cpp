#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

enum class Mode { Auto, Xbox, PlayStation, Nintendo };
enum class Family { Unknown, Xbox, PlayStation, Nintendo };

struct CommandResult {
    bool present = false;
    Mode mode = Mode::Auto;
};

CommandResult Parse(const std::vector<std::wstring>& argv) {
    CommandResult result{};
    for (std::size_t i = 1; i < argv.size(); ++i) {
        Mode mode = Mode::Auto;
        bool matched = true;
        if (wcscasecmp(argv[i].c_str(), L"-ps") == 0) mode = Mode::PlayStation;
        else if (wcscasecmp(argv[i].c_str(), L"-ns") == 0) mode = Mode::Nintendo;
        else if (wcscasecmp(argv[i].c_str(), L"-xb") == 0) mode = Mode::Xbox;
        else if (wcscasecmp(argv[i].c_str(), L"-auto") == 0) mode = Mode::Auto;
        else matched = false;
        if (matched) {
            result.present = true;
            result.mode = mode;
        }
    }
    return result;
}

std::string Normalize(const char* text) {
    std::string out;
    if (!text) return out;
    while (*text) {
        const unsigned char ch = static_cast<unsigned char>(*text++);
        if (std::isalnum(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

bool Starts(const std::string& value, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    return value.size() >= n && value.compare(0, n, prefix) == 0;
}

Family KnownType(int type) {
    switch (type) {
    case 2: case 3: return Family::Xbox;
    case 4: case 5: case 6: return Family::PlayStation;
    case 7: case 8: case 9: case 10: case 11: return Family::Nintendo;
    default: return Family::Unknown;
    }
}

Family TypeString(const char* raw) {
    const auto token = Normalize(raw);
    if (Starts(token, "playstation") || Starts(token, "dualshock") ||
        Starts(token, "dualsense") ||
        (Starts(token, "ps") && token.size() >= 3 &&
         std::isdigit(static_cast<unsigned char>(token[2])))) return Family::PlayStation;
    if (Starts(token, "xbox") || Starts(token, "xinput")) return Family::Xbox;
    if (Starts(token, "nintendo") || Starts(token, "switch") ||
        Starts(token, "joycon") || Starts(token, "gamecube") ||
        Starts(token, "ns2") || Starts(token, "nspro")) return Family::Nintendo;
    return Family::Unknown;
}

Family Labels(int south, int east, int west, int north) {
    if (south == 5 && east == 6 && west == 7 && north == 8) return Family::PlayStation;
    if (south == 2 && east == 1 && west == 4 && north == 3) return Family::Nintendo;
    if (south == 1 && east == 2 && west == 3 && north == 4) return Family::Xbox;
    return Family::Unknown;
}

Family Vendor(std::uint16_t vid) {
    switch (vid) {
    case 0x054C: return Family::PlayStation;
    case 0x057E: return Family::Nintendo;
    case 0x045E: return Family::Xbox;
    default: return Family::Unknown;
    }
}

std::array<std::uint32_t, 4> Reconcile(
    std::array<std::uint32_t, 4> slots,
    const std::vector<std::uint32_t>& connected) {
    auto contains = [&](std::uint32_t id) {
        return std::find(connected.begin(), connected.end(), id) != connected.end();
    };
    for (auto& id : slots) if (id != 0 && !contains(id)) id = 0;
    for (const auto id : connected) {
        if (id == 0 || std::find(slots.begin(), slots.end(), id) != slots.end()) continue;
        const auto empty = std::find(slots.begin(), slots.end(), 0u);
        if (empty == slots.end()) break;
        *empty = id;
    }
    return slots;
}

int main() {
    assert(!Parse({L"Bond.exe"}).present);
    assert(Parse({L"Bond.exe", L"-PS"}).mode == Mode::PlayStation);
    assert(Parse({L"Bond.exe", L"-ps", L"-ns"}).mode == Mode::Nintendo);
    assert(Parse({L"Bond.exe", L"-ns", L"-auto"}).mode == Mode::Auto);
    assert(!Parse({L"Bond.exe", L"-something-ps-test"}).present);

    assert(KnownType(2) == Family::Xbox);
    assert(KnownType(6) == Family::PlayStation);
    assert(KnownType(11) == Family::Nintendo);
    assert(KnownType(13) == Family::Unknown); // future enum must use later layers

    assert(TypeString("PS6") == Family::PlayStation);
    assert(TypeString("DualSense Edge") == Family::PlayStation);
    assert(TypeString("XboxNext") == Family::Xbox);
    assert(TypeString("Switch 2 Pro") == Family::Nintendo);
    assert(TypeString("NS2Pro") == Family::Nintendo);
    assert(TypeString("future-generic") == Family::Unknown);

    assert(Labels(5, 6, 7, 8) == Family::PlayStation);
    assert(Labels(2, 1, 4, 3) == Family::Nintendo);
    assert(Labels(1, 2, 3, 4) == Family::Xbox);
    assert(Vendor(0x054C) == Family::PlayStation);
    assert(Vendor(0x057E) == Family::Nintendo);
    assert(Vendor(0x045E) == Family::Xbox);

    auto slots = Reconcile({}, {10, 20});
    assert((slots == std::array<std::uint32_t, 4>{10, 20, 0, 0}));
    slots = Reconcile(slots, {20, 30});
    // 20 remains in slot 1; new 30 fills the now-empty slot 0.
    assert((slots == std::array<std::uint32_t, 4>{30, 20, 0, 0}));
    slots = Reconcile(slots, {20});
    assert((slots == std::array<std::uint32_t, 4>{0, 20, 0, 0}));
    slots = Reconcile(slots, {20, 40});
    assert((slots == std::array<std::uint32_t, 4>{40, 20, 0, 0}));

    std::cout << "auto family harness passed: cli=exact_last_wins known=explicit future=string "
                 "labels=PS_NS_Xbox vendor=fallback slots=retain_lowest_empty slot0=tracked\n";
}
