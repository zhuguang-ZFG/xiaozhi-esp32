#pragma once

// 速度只影响 max-rate，不改变云端 G-code 进给与机型安全金表。
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace hutuji {
inline constexpr int kMachineSpeedMin = 3000;
inline constexpr int kMachineSpeedMax = 16000;
inline constexpr int kMachineSpeedDefault = 10000;
// Z 为弹簧笔架，保留现值；细调范围独立于 XY，3000 是本轮软件上限。
inline constexpr int kPenSpeedMin = 100;
inline constexpr int kPenSpeedMax = 3000;

enum class MachineSpeedAxis { Invalid, X, Y, Z, XY };

inline MachineSpeedAxis ParseMachineSpeedAxis(std::string_view axis) {
    if (axis == "x")
        return MachineSpeedAxis::X;
    if (axis == "y")
        return MachineSpeedAxis::Y;
    if (axis == "z")
        return MachineSpeedAxis::Z;
    if (axis == "xy")
        return MachineSpeedAxis::XY;
    return MachineSpeedAxis::Invalid;
}

inline bool SpeedIncludesAxis(MachineSpeedAxis axis, int setting) {
    return (setting == 110 && (axis == MachineSpeedAxis::X || axis == MachineSpeedAxis::XY)) ||
           (setting == 111 && (axis == MachineSpeedAxis::Y || axis == MachineSpeedAxis::XY)) ||
           (setting == 112 && axis == MachineSpeedAxis::Z);
}

inline bool IsMachineSpeedValid(int rate, MachineSpeedAxis axis = MachineSpeedAxis::XY) {
    if (axis == MachineSpeedAxis::Invalid)
        return false;
    return axis == MachineSpeedAxis::Z ? rate >= kPenSpeedMin && rate <= kPenSpeedMax
                                       : rate >= kMachineSpeedMin && rate <= kMachineSpeedMax;
}

inline bool IsSpeedSettledState(std::string_view state) {
    return state == "idle" || state == "done" || state == "error" || state == "aborted";
}

// 不从配置默认值伪造读回；每轴序号证明本次查询实际收到了参数行。
struct MachineSpeedSnapshot {
    std::array<double, 3> rates{};
    std::array<uint32_t, 3> revisions{};

    void Observe(std::string_view key, double value) {
        if (!std::isfinite(value) || value <= 0)
            return;
        const int index = key == "110" ? 0 : key == "111" ? 1 : key == "112" ? 2 : -1;
        if (index < 0)
            return;
        rates[index] = value;
        ++revisions[index];
    }

    bool Complete() const {
        for (double rate : rates) {
            if (!std::isfinite(rate) || rate <= 0)
                return false;
        }
        return true;
    }
};

inline bool SpeedReadbackMatches(MachineSpeedAxis axis, int rate,
                                 const MachineSpeedSnapshot& snapshot) {
    if (axis == MachineSpeedAxis::Invalid || !snapshot.Complete())
        return false;
    if (rate == 0)
        return true;
    for (int setting = 110; setting <= 112; ++setting) {
        if (SpeedIncludesAxis(axis, setting) && snapshot.rates[setting - 110] != rate)
            return false;
    }
    return true;
}

enum class SpeedWriteResult { Ok, InvalidAxis, SessionLost, XFailed, YFailed, ZFailed };

// 只写选中轴；旧调用默认 XY。每次应答后复核连接，重连不续写半个事务。
template <typename SessionReady, typename WriteAxis>
SpeedWriteResult WriteMachineSpeed(SessionReady same_session, WriteAxis write_axis,
                                   MachineSpeedAxis selected = MachineSpeedAxis::XY) {
    if (selected == MachineSpeedAxis::Invalid)
        return SpeedWriteResult::InvalidAxis;
    for (int axis = 110; axis <= 112; ++axis) {
        if (!SpeedIncludesAxis(selected, axis))
            continue;
        if (!same_session())
            return SpeedWriteResult::SessionLost;
        const bool ok = write_axis(axis);
        if (!same_session())
            return SpeedWriteResult::SessionLost;
        if (!ok) {
            return axis == 110   ? SpeedWriteResult::XFailed
                   : axis == 111 ? SpeedWriteResult::YFailed
                                 : SpeedWriteResult::ZFailed;
        }
    }
    return SpeedWriteResult::Ok;
}
}  // namespace hutuji
