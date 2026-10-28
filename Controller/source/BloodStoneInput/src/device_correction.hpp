#pragma once

#include <cstdint>

namespace joyshim {

struct AxisCorrection {
    bool swapXY{};
    bool invertX{};
    bool invertY{};
};

struct DeviceCorrection {
    AxisCorrection leftStick{};
    AxisCorrection rightStick{};
};

// Profile 输出层二维线性矩阵。系数使用百分比：单位矩阵为
// [100, 0; 0, 100]。未配置时不会改变任何摇杆值。
//
// Xout = (XX * X + XY * Y) / 100
// Yout = (YX * X + YY * Y) / 100
struct AxisMatrix {
    std::int32_t xx{100};
    std::int32_t xy{};
    std::int32_t yx{};
    std::int32_t yy{100};
};

// 少数旧游戏会在两轴绝对值完全相等或非常接近时进入错误分支。
// 该修正只在显式 Profile 中启用：当两轴均离开中心且绝对值差不超过
// tolerance 时，在指定轴上制造一个很小的差值。默认关闭，不参与透明路径。
enum class DiagonalBiasAxis {
    X,
    Y,
};

enum class DiagonalSignMode {
    All,
    SameSigns,
    OppositeSigns,
};

struct DiagonalCorrection {
    bool enabled{};
    std::int32_t tolerance{};
    std::int32_t offset{};
    // 可选的更强修正：仅在两轴都达到 minimumMagnitude、且两轴幅值差
    // 不超过 tolerance 时，强制指定主轴至少领先 minimumGap。值为 0 时
    // 完全沿用旧的 offset 行为，因此不会改变既有 Profile 的语义。
    std::int32_t minimumMagnitude{};
    std::int32_t minimumGap{};
    DiagonalBiasAxis axis{DiagonalBiasAxis::X};
    DiagonalSignMode signMode{DiagonalSignMode::All};
};

// 对指定符号象限执行“同时双轴分离”测试。它与对角线差值修正不同：
// 不再只处理接近 45 度的区域，而是可以在整个目标象限内压低一个轴。
// 该功能仅由显式 Profile 启用，默认 None，不进入透明转换路径。
enum class AxisSeparationMode {
    None,
    PreferX,
    PreferY,
    DominantAxis,
};

struct AxisSeparationCorrection {
    AxisSeparationMode mode{AxisSeparationMode::None};
    std::int32_t minimumMagnitude{};
    // 被压低轴保留的百分比。0 表示纯单轴，100 表示不改变。
    std::int32_t secondaryPercent{};
    DiagonalSignMode signMode{DiagonalSignMode::All};
};

struct AxisOutputTuning {
    AxisMatrix matrix{};
    DiagonalCorrection diagonal{};
    AxisSeparationCorrection separation{};
};

struct DeviceOutputTuning {
    AxisOutputTuning leftStick{};
    AxisOutputTuning rightStick{};
};

struct AxisPair {
    std::int16_t x{};
    std::int16_t y{};
};

constexpr std::int16_t NegateAxis(std::int16_t value) noexcept {
    return value == static_cast<std::int16_t>(-32768)
        ? static_cast<std::int16_t>(32767)
        : static_cast<std::int16_t>(-value);
}

constexpr AxisPair ApplyAxisCorrection(
    std::int16_t x,
    std::int16_t y,
    const AxisCorrection& correction) noexcept {
    AxisPair result{x, y};
    if (correction.swapXY) {
        const std::int16_t temporary = result.x;
        result.x = result.y;
        result.y = temporary;
    }
    if (correction.invertX) result.x = NegateAxis(result.x);
    if (correction.invertY) result.y = NegateAxis(result.y);
    return result;
}

constexpr std::int64_t DividePercentRounded(std::int64_t value) noexcept {
    return value >= 0 ? (value + 50) / 100 : (value - 50) / 100;
}

constexpr std::int16_t ClampAxis(std::int64_t value) noexcept {
    return value > 32767
        ? static_cast<std::int16_t>(32767)
        : value < -32768
            ? static_cast<std::int16_t>(-32768)
            : static_cast<std::int16_t>(value);
}

constexpr std::int64_t AxisMagnitude(std::int16_t value) noexcept {
    return value == static_cast<std::int16_t>(-32768)
        ? 32768
        : (value < 0 ? -static_cast<std::int64_t>(value) : value);
}

constexpr std::int16_t ApplySignedMagnitude(
    std::int16_t original,
    std::int64_t magnitude) noexcept {
    if (magnitude < 0) magnitude = 0;
    if (original < 0) return ClampAxis(-magnitude);
    return ClampAxis(magnitude);
}

constexpr AxisPair ApplyAxisMatrix(
    std::int16_t x,
    std::int16_t y,
    const AxisMatrix& matrix) noexcept {
    const std::int64_t sourceX = x;
    const std::int64_t sourceY = y;
    const std::int64_t mixedX =
        static_cast<std::int64_t>(matrix.xx) * sourceX +
        static_cast<std::int64_t>(matrix.xy) * sourceY;
    const std::int64_t mixedY =
        static_cast<std::int64_t>(matrix.yx) * sourceX +
        static_cast<std::int64_t>(matrix.yy) * sourceY;
    return AxisPair{
        ClampAxis(DividePercentRounded(mixedX)),
        ClampAxis(DividePercentRounded(mixedY)),
    };
}

constexpr AxisPair ApplyDiagonalCorrection(
    std::int16_t x,
    std::int16_t y,
    const DiagonalCorrection& correction) noexcept {
    if (!correction.enabled ||
        (correction.offset <= 0 && correction.minimumGap <= 0) ||
        x == 0 || y == 0) {
        return AxisPair{x, y};
    }
    const bool sameSigns = (x < 0) == (y < 0);
    if ((correction.signMode == DiagonalSignMode::SameSigns && !sameSigns) ||
        (correction.signMode == DiagonalSignMode::OppositeSigns && sameSigns)) {
        return AxisPair{x, y};
    }
    const std::int64_t magnitudeX = AxisMagnitude(x);
    const std::int64_t magnitudeY = AxisMagnitude(y);
    const std::int64_t difference = magnitudeX >= magnitudeY
        ? magnitudeX - magnitudeY : magnitudeY - magnitudeX;
    if (difference > correction.tolerance) return AxisPair{x, y};

    AxisPair result{x, y};

    // minimumGap 用于处理比“精确 45 度”更宽的故障区域。它只在指定
    // 象限、两轴都明显离开中心且接近对角线时生效；不会影响中心区、
    // 水平/垂直方向或其他符号象限。该模式不会改变默认透明路径。
    if (correction.minimumGap > 0) {
        if (magnitudeX < correction.minimumMagnitude ||
            magnitudeY < correction.minimumMagnitude) {
            return result;
        }
        const std::int64_t gap = correction.minimumGap > 32767
            ? 32767 : correction.minimumGap;
        if (difference >= gap) return result;

        if (correction.axis == DiagonalBiasAxis::X) {
            const std::int64_t wantedX = magnitudeY + gap;
            if (wantedX <= 32767) {
                result.x = ApplySignedMagnitude(x, wantedX);
            } else {
                result.x = ApplySignedMagnitude(x, 32767);
                result.y = ApplySignedMagnitude(y, 32767 - gap);
            }
        } else {
            const std::int64_t wantedY = magnitudeX + gap;
            if (wantedY <= 32767) {
                result.y = ApplySignedMagnitude(y, wantedY);
            } else {
                result.y = ApplySignedMagnitude(y, 32767);
                result.x = ApplySignedMagnitude(x, 32767 - gap);
            }
        }
        return result;
    }

    if (correction.offset <= 0) return result;
    if (correction.axis == DiagonalBiasAxis::X) {
        if (magnitudeX + correction.offset <= 32767) {
            result.x = ApplySignedMagnitude(x, magnitudeX + correction.offset);
        } else {
            result.y = ApplySignedMagnitude(y, magnitudeY - correction.offset);
        }
    } else {
        if (magnitudeY + correction.offset <= 32767) {
            result.y = ApplySignedMagnitude(y, magnitudeY + correction.offset);
        } else {
            result.x = ApplySignedMagnitude(x, magnitudeX - correction.offset);
        }
    }
    return result;
}

constexpr std::int16_t ScaleAxisPercent(
    std::int16_t value,
    std::int32_t percent) noexcept {
    if (percent <= 0) return 0;
    if (percent >= 100) return value;
    return ClampAxis(DividePercentRounded(
        static_cast<std::int64_t>(value) * percent));
}

constexpr AxisPair ApplyAxisSeparation(
    std::int16_t x,
    std::int16_t y,
    const AxisSeparationCorrection& correction) noexcept {
    if (correction.mode == AxisSeparationMode::None || x == 0 || y == 0) {
        return AxisPair{x, y};
    }
    const bool sameSigns = (x < 0) == (y < 0);
    if ((correction.signMode == DiagonalSignMode::SameSigns && !sameSigns) ||
        (correction.signMode == DiagonalSignMode::OppositeSigns && sameSigns)) {
        return AxisPair{x, y};
    }
    const std::int64_t magnitudeX = AxisMagnitude(x);
    const std::int64_t magnitudeY = AxisMagnitude(y);
    if (magnitudeX < correction.minimumMagnitude ||
        magnitudeY < correction.minimumMagnitude) {
        return AxisPair{x, y};
    }

    AxisPair result{x, y};
    switch (correction.mode) {
    case AxisSeparationMode::PreferX:
        result.y = ScaleAxisPercent(y, correction.secondaryPercent);
        break;
    case AxisSeparationMode::PreferY:
        result.x = ScaleAxisPercent(x, correction.secondaryPercent);
        break;
    case AxisSeparationMode::DominantAxis:
        if (magnitudeX >= magnitudeY) {
            result.y = ScaleAxisPercent(y, correction.secondaryPercent);
        } else {
            result.x = ScaleAxisPercent(x, correction.secondaryPercent);
        }
        break;
    case AxisSeparationMode::None:
        break;
    }
    return result;
}

constexpr AxisPair ApplyAxisOutputTuning(
    std::int16_t x,
    std::int16_t y,
    const AxisOutputTuning& tuning) noexcept {
    const AxisPair mixed = ApplyAxisMatrix(x, y, tuning.matrix);
    const AxisPair diagonal = ApplyDiagonalCorrection(mixed.x, mixed.y, tuning.diagonal);
    return ApplyAxisSeparation(diagonal.x, diagonal.y, tuning.separation);
}

constexpr bool HasCorrection(const DeviceCorrection& correction) noexcept {
    return correction.leftStick.swapXY || correction.leftStick.invertX || correction.leftStick.invertY ||
        correction.rightStick.swapXY || correction.rightStick.invertX || correction.rightStick.invertY;
}

constexpr bool IsIdentityMatrix(const AxisMatrix& matrix) noexcept {
    return matrix.xx == 100 && matrix.xy == 0 && matrix.yx == 0 && matrix.yy == 100;
}

constexpr bool HasOutputTuning(const AxisOutputTuning& tuning) noexcept {
    return !IsIdentityMatrix(tuning.matrix) || tuning.diagonal.enabled ||
        tuning.separation.mode != AxisSeparationMode::None;
}

constexpr bool HasOutputTuning(const DeviceOutputTuning& tuning) noexcept {
    return HasOutputTuning(tuning.leftStick) || HasOutputTuning(tuning.rightStick);
}

}  // namespace joyshim
