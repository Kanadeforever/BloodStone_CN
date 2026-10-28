#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "device_correction.hpp"

int main() {
    using namespace joyshim;

    AxisCorrection none{};
    { const AxisPair value = ApplyAxisCorrection(100, -200, none); assert(value.x == 100 && value.y == -200); }

    AxisCorrection swap{};
    swap.swapXY = true;
    { const AxisPair value = ApplyAxisCorrection(100, -200, swap); assert(value.x == -200 && value.y == 100); }

    AxisCorrection invert{};
    invert.invertX = true;
    invert.invertY = true;
    { const AxisPair value = ApplyAxisCorrection(100, -200, invert); assert(value.x == -100 && value.y == 200); }
    assert(ApplyAxisCorrection(-32768, -32768, invert).x == 32767);
    assert(ApplyAxisCorrection(-32768, -32768, invert).y == 32767);

    DeviceCorrection device{};
    assert(!HasCorrection(device));
    device.rightStick.invertY = true;
    assert(HasCorrection(device));

    AxisMatrix identity{};
    { const AxisPair value = ApplyAxisMatrix(12345, -23456, identity); assert(value.x == 12345 && value.y == -23456); }
    assert(IsIdentityMatrix(identity));

    AxisMatrix swapMatrix{};
    swapMatrix.xx = 0;
    swapMatrix.xy = 100;
    swapMatrix.yx = 100;
    swapMatrix.yy = 0;
    { const AxisPair value = ApplyAxisMatrix(1000, -2000, swapMatrix); assert(value.x == -2000 && value.y == 1000); }

    DiagonalCorrection disabled{};
    disabled.offset = 512;
    { const AxisPair value = ApplyDiagonalCorrection(10000, -10000, disabled); assert(value.x == 10000 && value.y == -10000); }

    DiagonalCorrection diagonal{};
    diagonal.enabled = true;
    diagonal.tolerance = 128;
    diagonal.offset = 512;
    diagonal.axis = DiagonalBiasAxis::X;
    diagonal.signMode = DiagonalSignMode::OppositeSigns;
    { const AxisPair value = ApplyDiagonalCorrection(10000, -10000, diagonal); assert(value.x == 10512 && value.y == -10000); }
    { const AxisPair value = ApplyDiagonalCorrection(-10000, 10050, diagonal); assert(value.x == -10512 && value.y == 10050); }
    { const AxisPair value = ApplyDiagonalCorrection(10000, 10000, diagonal); assert(value.x == 10000 && value.y == 10000); }
    { const AxisPair value = ApplyDiagonalCorrection(10000, 12000, diagonal); assert(value.x == 10000 && value.y == 12000); }
    diagonal.signMode = DiagonalSignMode::All;
    { const AxisPair value = ApplyDiagonalCorrection(32767, 32767, diagonal); assert(value.x == 32767 && value.y == 32255); }

    DiagonalCorrection minimumGap{};
    minimumGap.enabled = true;
    minimumGap.tolerance = 8192;
    minimumGap.minimumMagnitude = 4096;
    minimumGap.minimumGap = 4096;
    minimumGap.axis = DiagonalBiasAxis::X;
    minimumGap.signMode = DiagonalSignMode::OppositeSigns;
    { const AxisPair value = ApplyDiagonalCorrection(-20000, 20000, minimumGap); assert(value.x == -24096 && value.y == 20000); }
    { const AxisPair value = ApplyDiagonalCorrection(20000, -23000, minimumGap); assert(value.x == 27096 && value.y == -23000); }
    { const AxisPair value = ApplyDiagonalCorrection(-20000, -20000, minimumGap); assert(value.x == -20000 && value.y == -20000); }
    { const AxisPair value = ApplyDiagonalCorrection(-3000, 3000, minimumGap); assert(value.x == -3000 && value.y == 3000); }
    { const AxisPair value = ApplyDiagonalCorrection(-10000, 25000, minimumGap); assert(value.x == -10000 && value.y == 25000); }
    { const AxisPair value = ApplyDiagonalCorrection(-32767, 32767, minimumGap); assert(value.x == -32767 && value.y == 28671); }

    AxisSeparationCorrection separation{};
    separation.mode = AxisSeparationMode::PreferX;
    separation.minimumMagnitude = 4096;
    separation.secondaryPercent = 0;
    separation.signMode = DiagonalSignMode::OppositeSigns;
    { const AxisPair value = ApplyAxisSeparation(-27349, 20899, separation); assert(value.x == -27349 && value.y == 0); }
    { const AxisPair value = ApplyAxisSeparation(26832, -17286, separation); assert(value.x == 26832 && value.y == 0); }
    { const AxisPair value = ApplyAxisSeparation(23478, 22447, separation); assert(value.x == 23478 && value.y == 22447); }
    separation.mode = AxisSeparationMode::PreferY;
    { const AxisPair value = ApplyAxisSeparation(-27349, 20899, separation); assert(value.x == 0 && value.y == 20899); }
    separation.mode = AxisSeparationMode::DominantAxis;
    separation.secondaryPercent = 25;
    { const AxisPair value = ApplyAxisSeparation(-20000, 24000, separation); assert(value.x == -5000 && value.y == 24000); }
    { const AxisPair value = ApplyAxisSeparation(3000, -20000, separation); assert(value.x == 3000 && value.y == -20000); }

    AxisOutputTuning tuning{};
    assert(!HasOutputTuning(tuning));
    diagonal.signMode = DiagonalSignMode::SameSigns;
    tuning.diagonal = diagonal;
    assert(HasOutputTuning(tuning));
    { const AxisPair value = ApplyAxisOutputTuning(-16000, -16000, tuning); assert(value.x == -16512 && value.y == -16000); }
    tuning = {};
    tuning.separation = separation;
    tuning.separation.mode = AxisSeparationMode::PreferX;
    tuning.separation.secondaryPercent = 0;
    { const AxisPair value = ApplyAxisOutputTuning(26000, -18000, tuning); assert(value.x == 26000 && value.y == 0); }

    return 0;
}
