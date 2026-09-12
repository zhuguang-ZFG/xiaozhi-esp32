#pragma once

// 速度事务只调机型允许的 max-rate；范围不改变云端 G-code 进给契约。
#include <string_view>

namespace hutuji {
inline constexpr int kMachineSpeedMin = 3000;
inline constexpr int kMachineSpeedMax = 16000;

inline bool IsMachineSpeedValid(int rate) {
    return rate >= kMachineSpeedMin && rate <= kMachineSpeedMax;
}

inline bool IsSpeedSettledState(std::string_view state) {
    return state == "idle" || state == "done" || state == "error" || state == "aborted";
}

enum class SpeedWriteResult { Ok, SessionLost, XFailed, YFailed };

// 两轴之间与每条应答之后都核对同一会话；重连不能续写余下半个事务。
template <typename SessionReady, typename WriteAxis>
SpeedWriteResult WriteMachineSpeed(SessionReady same_session, WriteAxis write_axis) {
    for (int axis = 110; axis <= 111; ++axis) {
        if (!same_session())
            return SpeedWriteResult::SessionLost;
        const bool ok = write_axis(axis);
        if (!same_session())
            return SpeedWriteResult::SessionLost;
        if (!ok)
            return axis == 110 ? SpeedWriteResult::XFailed : SpeedWriteResult::YFailed;
    }
    return SpeedWriteResult::Ok;
}
}  // namespace hutuji
