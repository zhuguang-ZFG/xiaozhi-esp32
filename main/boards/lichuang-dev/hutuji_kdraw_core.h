#pragma once

// §10.4.17：只有同一已知写字机的连续新包才能证明完成；超时从不推进完成。
#include <cstdint>
#include <limits>
#include <string_view>

namespace hutuji::kdraw {
enum class BeaconState { Run, Idle, Interrupted, Other };
struct Beacon {
    BeaconState state = BeaconState::Other;
    bool changing = true;
    uint32_t sequence = 0;
};

inline bool ParseBeacon(std::string_view line, Beacon& output) {
    if (line.size() < 3 || line.front() != '<' || line.back() != '>')
        return false;
    line.remove_prefix(1);
    line.remove_suffix(1);
    const size_t first = line.find('|');
    if (first == std::string_view::npos)
        return false;
    const auto state = line.substr(0, first);
    Beacon parsed;
    if (state == "Run")
        parsed.state = BeaconState::Run;
    else if (state == "Idle")
        parsed.state = BeaconState::Idle;
    else if (state == "Alarm" || state == "Check")
        parsed.state = BeaconState::Interrupted;
    else if (state != "Hold:0" && state != "Hold:1" && state != "Door:0" && state != "Door:1" &&
             state != "Door:2" && state != "Door:3" && state != "Jog" && state != "Home" &&
             state != "Sleep")
        return false;
    bool have_changing = false;
    bool have_sequence = false;
    size_t start = first + 1;
    while (start <= line.size()) {
        size_t end = line.find('|', start);
        if (end == std::string_view::npos)
            end = line.size();
        const auto field = line.substr(start, end - start);
        if (field.substr(0, 9) == "Changing=") {
            if (have_changing || (field.substr(9) != "On" && field.substr(9) != "Off"))
                return false;
            parsed.changing = field.substr(9) == "On";
            have_changing = true;
        } else if (field.substr(0, 4) == "Seq=") {
            if (have_sequence || field.size() == 4)
                return false;
            uint32_t seq = 0;
            for (char ch : field.substr(4)) {
                if (ch < '0' || ch > '9')
                    return false;
                const uint32_t digit = static_cast<uint32_t>(ch - '0');
                if (seq > (std::numeric_limits<uint32_t>::max() - digit) / 10)
                    return false;
                seq = seq * 10 + digit;
            }
            parsed.sequence = seq;
            have_sequence = true;
        } else
            return false;
        if (end == line.size())
            break;
        start = end + 1;
    }
    if (!have_changing || !have_sequence)
        return false;
    output = parsed;
    return true;
}

class CompletionTracker {
public:
    // 广播心跳 1s，稳定窗 2.5s；超过两拍加 0.5s 裕量视为失联。
    static constexpr uint64_t kStableMs = 2500;
    static constexpr uint64_t kStaleMs = 2500;

    void Expire(uint64_t now) {
        if (have_sequence_ && (now < last_received_ || now - last_received_ >= kStaleMs)) {
            ResetSession();
        }
    }

    bool Observe(uint32_t source, uint32_t expected, const Beacon& beacon, uint64_t now) {
        if (peer_ != expected) {
            ResetSession();
            peer_ = expected;
        }
        Expire(now);
        if (expected == 0 || source != expected)
            return false;
        if (have_sequence_) {
            const uint32_t delta = beacon.sequence - last_sequence_;
            if (delta == 0 || delta >= 0x80000000u)
                return false;
        }
        have_sequence_ = true;
        last_sequence_ = beacon.sequence;
        last_received_ = now;
        if (beacon.state == BeaconState::Run) {
            saw_run_ = true;
            idle_ = false;
        } else if (beacon.state == BeaconState::Interrupted) {
            saw_run_ = false;
            idle_ = false;
        } else if (saw_run_ && beacon.state == BeaconState::Idle && !beacon.changing) {
            if (!idle_) {
                idle_ = true;
                idle_since_ = now;
            } else if (now - idle_since_ >= kStableMs) {
                saw_run_ = false;
                idle_ = false;
                return true;
            }
        } else
            idle_ = false;
        return false;
    }

private:
    void ResetSession() {
        have_sequence_ = false;
        saw_run_ = false;
        idle_ = false;
    }
    uint32_t peer_ = 0;
    uint32_t last_sequence_ = 0;
    uint64_t last_received_ = 0;
    uint64_t idle_since_ = 0;
    bool have_sequence_ = false;
    bool saw_run_ = false;
    bool idle_ = false;
};
}  // namespace hutuji::kdraw
