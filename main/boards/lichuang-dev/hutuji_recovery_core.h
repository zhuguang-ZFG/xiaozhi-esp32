#ifndef HUTUJI_RECOVERY_CORE_H
#define HUTUJI_RECOVERY_CORE_H

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace hutuji {

/**
 * 单条发送的活动时间预算。部分成功不会续期；feed hold 仲裁挂起期间不计时，避免
 * 把等待实时字符的时间误判成 TCP 背压。全部计算使用无符号差值，覆盖 tick 回绕。
 */
inline constexpr uint32_t kSendStallBudgetMs = 20000;

class SendStallBudget {
public:
    constexpr SendStallBudget(uint32_t began, uint32_t budget) : began_(began), budget_(budget) {}

    constexpr void Suspend(uint32_t from, uint32_t to) {
        suspended_ += static_cast<uint32_t>(to - from);
    }

    constexpr bool Expired(uint32_t now) const {
        const uint32_t elapsed = static_cast<uint32_t>(now - began_);
        return static_cast<uint32_t>(elapsed - suspended_) >= budget_;
    }

private:
    uint32_t began_;
    uint32_t budget_;
    uint32_t suspended_ = 0;
};

inline constexpr bool ShouldYieldToFeedHold(bool feed_hold_priority, uint32_t waiters) {
    return !feed_hold_priority && waiters > 0;
}

/** 只有 send() 真正写出正数字节时才推进游标，错误/零进展必须留在原位。 */
inline constexpr bool AdvanceSendProgress(size_t& sent, int n) {
    if (n <= 0) {
        return false;
    }
    sent += static_cast<size_t>(n);
    return true;
}

/**
 * send() 返回 EAGAIN/EWOULDBLOCK 时是否应重试（而不是立即断开）。时间预算由
 * SendStallBudget 独立判定，避免错误谓词同时拥有时钟状态。
 * @param e 当前 errno 值（errno 是宏，不能作参数名）
 */
inline constexpr bool ShouldRetrySend(int n, int e) {
    return n < 0 && (e == EAGAIN || e == EWOULDBLOCK);
}

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
                                              std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool CancelClaim() {
        AbortResetOwnerPhase expected = AbortResetOwnerPhase::Running;
        return phase_.compare_exchange_strong(expected, AbortResetOwnerPhase::Idle,
                                              std::memory_order_acq_rel, std::memory_order_acquire);
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

/** 灌行前闸的决策结果；socket 故障由调用方单独处理。 */
enum class StreamSendCancel : uint8_t {
    Allowed = 0,
    Paused,
    Aborted,
};

/**
 * 普通灌行前闸判定：把 stream_mutex_ 内快照与发送前的最新原子读捏成一次决策。
 * 语义是「拒绝集 ≥ 持锁直接检查」：abort 判定改为「状态位置位，或控制纪元
 * 在快照后前进」。pause/abort 提交方在发布暂停/abort 状态的同时（同一把
 * stream_mutex_ 内）递增 stream_control_epoch_，因此快照后发生的提交必然被
 * 快照 epoch 捕获。换纸行分支仍在 stream_mutex_ 内检查 paused，abort 则由循环顶
 * 的排流逻辑兜底。
 */
inline constexpr StreamSendCancel DecideStreamSend(bool snap_abort, bool snap_paused,
                                                   uint32_t snap_epoch, uint32_t latest_epoch) {
    if (snap_abort || latest_epoch != snap_epoch) {
        return StreamSendCancel::Aborted;
    }
    return snap_paused ? StreamSendCancel::Paused : StreamSendCancel::Allowed;
}

/** pause/abort 提交方的纪元递增（允许回绕；ABA 需 2^32 次提交，实际不可达）。 */
inline constexpr uint32_t NextStreamControlEpoch(uint32_t epoch) { return epoch + 1U; }

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
                !IsAfter(receive_epoch, expected_receive_epoch_.load(std::memory_order_relaxed))) {
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
