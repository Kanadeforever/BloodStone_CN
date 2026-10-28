#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace joyshim {

enum class ConnectionKind {
    Unknown,
    Wired,
    Wireless,
};

enum class ControllerMatchKind : std::uint8_t {
    None = 0,
    Serial = 1,
    Path = 2,
    DeviceId = 3,
    Name = 4,
    NameContains = 5,
    GuidFallback = 6,
    MatchAny = 7,
};

constexpr std::size_t MaxControllerGuids = 16;
constexpr std::size_t MaxControllerTextMatches = 16;
constexpr std::size_t MaxControllerDeviceIds = 32;
constexpr std::size_t MaxDeviceText = 256;
constexpr std::size_t MaxDevicePath = 512;

struct DeviceIdValue {
    std::uint16_t vendor{};
    std::uint16_t product{};
};

struct DeviceTextValue {
    char value[MaxDeviceText]{};
};

struct DevicePathValue {
    char value[MaxDevicePath]{};
};

struct DeviceIdentity {
    std::uint16_t vendor{};
    std::uint16_t product{};
    ConnectionKind connection{ConnectionKind::Unknown};
    char guid[33]{};
    char name[MaxDeviceText]{};
    char serial[MaxDeviceText]{};
    char path[MaxDevicePath]{};
};

struct DirectInputDeviceIdentity {
    std::uint16_t vendor{};
    std::uint16_t product{};
    char name[MaxDeviceText]{};
};

struct ControllerMatchFields {
    bool enabled{};
    bool matchAny{};
    std::vector<std::array<char, 33>> guids{};
    std::vector<DeviceTextValue> names{};
    std::vector<DeviceTextValue> nameContains{};
    std::vector<DeviceTextValue> serials{};
    std::vector<DevicePathValue> paths{};
    std::vector<DeviceIdValue> deviceIds{};
    std::vector<ConnectionKind> connections{};
};

ControllerMatchKind EvaluateControllerMatch(
    const ControllerMatchFields& controller,
    const DeviceIdentity& identity) noexcept;

ControllerMatchKind EvaluateDirectInputControllerMatch(
    const ControllerMatchFields& controller,
    const DirectInputDeviceIdentity& identity) noexcept;

const wchar_t* ControllerMatchKindName(ControllerMatchKind kind) noexcept;

constexpr unsigned int ControllerMatchRank(ControllerMatchKind kind) noexcept {
    return static_cast<unsigned int>(kind);
}

}  // namespace joyshim
