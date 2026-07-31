#ifndef HUTUJI_RECOVERY_CORE_H
#define HUTUJI_RECOVERY_CORE_H

#include <atomic>
#include <cstdint>
#include <limits>

namespace hutuji {

inline constexpr bool IsStoppedForReset(bool idle, bool hold, bool hold_complete) {
    return idle || (hold && hold_complete);
}

inline constexpr bool CanSendAbortReset(bool connected, bool ready, bool same_session,
                                        bool fresh_stopped_status, bool fresh_paper_status,
                                        bool changing_off) {
    return connected && ready && same_session && fresh_stopped_status && fresh_paper_status &&
           changing_off;
}

class AbortResetToken {
public:
    bool Arm(uint32_t connection_generation) {
        if (connection_generation == std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        uint32_t expected = 0;
        return value_.compare_exchange_strong(expected, connection_generation + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    bool Consume(uint32_t connection_generation) {
        const uint32_t value = value_.exchange(0, std::memory_order_acq_rel);
        return value != 0 && connection_generation != std::numeric_limits<uint32_t>::max() &&
               value == connection_generation + 1;
    }

    void Cancel() { value_.store(0, std::memory_order_release); }

    bool Pending() const { return value_.load(std::memory_order_acquire) != 0; }

private:
    std::atomic<uint32_t> value_{0};
};

}  // namespace hutuji

#endif  // HUTUJI_RECOVERY_CORE_H
