#ifndef HUTUJI_RECOVERY_CORE_H
#define HUTUJI_RECOVERY_CORE_H

#include <atomic>
#include <cstdint>

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

inline constexpr bool IsResetSessionReady(bool connected, bool ready, bool task_session_active,
                                          bool allow_unready_reconnect) {
    return connected && (ready || (allow_unready_reconnect && task_session_active));
}

enum class AbortResetOwnerPhase : uint8_t {
    Idle = 0,
    Running,
    Succeeded,
    Failed,
};

class AbortResetOwner {
public:
    bool TryClaim() {
        AbortResetOwnerPhase expected = AbortResetOwnerPhase::Idle;
        return phase_.compare_exchange_strong(expected, AbortResetOwnerPhase::Running,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    bool CancelClaim() {
        AbortResetOwnerPhase expected = AbortResetOwnerPhase::Running;
        return phase_.compare_exchange_strong(expected, AbortResetOwnerPhase::Idle,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    bool Complete(bool success) {
        AbortResetOwnerPhase expected = AbortResetOwnerPhase::Running;
        return phase_.compare_exchange_strong(
            expected, success ? AbortResetOwnerPhase::Succeeded : AbortResetOwnerPhase::Failed,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool FailIfRunning() { return Complete(false); }

    bool ResetIfSettled() {
        AbortResetOwnerPhase current = phase_.load(std::memory_order_acquire);
        while (current != AbortResetOwnerPhase::Running) {
            if (phase_.compare_exchange_weak(current, AbortResetOwnerPhase::Idle,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    bool Running() const {
        return phase_.load(std::memory_order_acquire) == AbortResetOwnerPhase::Running;
    }

    bool Started() const {
        return phase_.load(std::memory_order_acquire) != AbortResetOwnerPhase::Idle;
    }

    bool Succeeded() const {
        return phase_.load(std::memory_order_acquire) == AbortResetOwnerPhase::Succeeded;
    }

    AbortResetOwnerPhase Phase() const { return phase_.load(std::memory_order_acquire); }

private:
    std::atomic<AbortResetOwnerPhase> phase_{AbortResetOwnerPhase::Idle};
};

enum class StreamQuiescence : uint8_t {
    Idle = 0,
    Active,
    Quiesced,
    Failed,
};

inline constexpr bool CanResetAfterStream(StreamQuiescence state) {
    return state == StreamQuiescence::Idle || state == StreamQuiescence::Quiesced;
}


inline constexpr StreamQuiescence FinishStream(bool proven_quiesced) {
    return proven_quiesced ? StreamQuiescence::Quiesced : StreamQuiescence::Failed;
}
class AbortResetToken {
public:
    /** 调用方必须用同一把发送锁串行化 Arm/Consume/Cancel。 */
    bool Arm(uint32_t connection_generation, uint32_t current_banner_generation,
             uint32_t current_receive_epoch) {
        const uint32_t expected_banner = NextGeneration(current_banner_generation);
        State state = state_.load(std::memory_order_acquire);
        while (true) {
            if (state == State::Arming || state == State::Pending) {
                return false;
            }
            // 已取消的 reset 结果仍可能迟到；同一 session/同一代不得重新布防，
            // 否则旧 banner 会兑现新 token（ABA）。换 session 或 banner 前进后才可重试。
            if (state == State::Cancelled &&
                connection_generation_.load(std::memory_order_relaxed) == connection_generation &&
                expected_banner_generation_.load(std::memory_order_relaxed) == expected_banner) {
                return false;
            }
            if (state_.compare_exchange_weak(state, State::Arming, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                break;
            }
        }
        connection_generation_.store(connection_generation, std::memory_order_relaxed);
        expected_banner_generation_.store(expected_banner, std::memory_order_relaxed);
        expected_receive_epoch_.store(current_receive_epoch, std::memory_order_relaxed);
        State expected = State::Arming;
        return state_.compare_exchange_strong(expected, State::Pending, std::memory_order_release,
                                              std::memory_order_acquire);
    }

    bool Consume(uint32_t connection_generation, uint32_t banner_generation,
                 uint32_t receive_epoch) {
        State state = state_.load(std::memory_order_acquire);
        while (state == State::Pending || state == State::Cancelled) {
            // 早到/旧 session banner 不得破坏当前 token；只消费精确绑定的一代。
            if (connection_generation_.load(std::memory_order_relaxed) != connection_generation ||
                expected_banner_generation_.load(std::memory_order_relaxed) != banner_generation ||
                !IsAfter(receive_epoch,
                         expected_receive_epoch_.load(std::memory_order_relaxed))) {
                return false;
            }
            const bool redeem = state == State::Pending;
            if (state_.compare_exchange_weak(state, State::Empty, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return redeem;
            }
        }
        return false;
    }

    void Cancel() {
        State state = state_.load(std::memory_order_acquire);
        while (state == State::Arming || state == State::Pending) {
            if (state_.compare_exchange_weak(state, State::Cancelled, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return;
            }
        }
    }

    bool Pending() const { return state_.load(std::memory_order_acquire) == State::Pending; }

    static constexpr uint32_t NextGeneration(uint32_t generation) { return generation + 1U; }

    /** RFC 1982 风格半区间比较，支持 UINT32_MAX→0。 */
    static constexpr bool IsAfter(uint32_t candidate, uint32_t baseline) {
        const uint32_t distance = candidate - baseline;
        return distance != 0U && distance < (uint32_t{1} << 31);
    }

private:
    enum class State : uint8_t {
        Empty = 0,
        Arming,
        Pending,
        Cancelled,
    };

    std::atomic<uint32_t> connection_generation_{0};
    std::atomic<uint32_t> expected_banner_generation_{0};
    std::atomic<uint32_t> expected_receive_epoch_{0};
    std::atomic<State> state_{State::Empty};
};

}  // namespace hutuji

#endif  // HUTUJI_RECOVERY_CORE_H
