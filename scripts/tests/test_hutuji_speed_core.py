"""三轴独立写入、实际读回和真实 worker 的失败收敛回归。"""
import unittest
import test_hutuji_nopaper_core as core


class HutujiSpeedCoreTest(unittest.TestCase):
    _compile_and_run = core.HutujiNopaperCoreTest._compile_and_run

    def test_speed_transaction(self):
        compiler = core.find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <vector>
#include "main/boards/lichuang-dev/hutuji_speed_core.h"
int main() {
    using namespace hutuji;
    assert(!IsMachineSpeedValid(0) && !IsMachineSpeedValid(2999));
    assert(IsMachineSpeedValid(3000) && IsMachineSpeedValid(16000));
    assert(!IsMachineSpeedValid(16001));
    assert(IsMachineSpeedValid(10000) && kMachineSpeedDefault == 10000);
    assert(IsMachineSpeedValid(100, MachineSpeedAxis::Z));
    assert(IsMachineSpeedValid(1200, MachineSpeedAxis::Z));
    assert(IsMachineSpeedValid(3000, MachineSpeedAxis::Z));
    assert(!IsMachineSpeedValid(99, MachineSpeedAxis::Z));
    assert(!IsMachineSpeedValid(3001, MachineSpeedAxis::Z));
    assert(!IsMachineSpeedValid(10000, MachineSpeedAxis::Invalid));
    assert(ParseMachineSpeedAxis("x") == MachineSpeedAxis::X);
    assert(ParseMachineSpeedAxis("y") == MachineSpeedAxis::Y);
    assert(ParseMachineSpeedAxis("z") == MachineSpeedAxis::Z);
    assert(ParseMachineSpeedAxis("xy") == MachineSpeedAxis::XY);
    assert(ParseMachineSpeedAxis("xyz") == MachineSpeedAxis::Invalid);
    assert(ParseMachineSpeedAxis("X") == MachineSpeedAxis::Invalid);
    assert(IsSpeedSettledState("done") && !IsSpeedSettledState("paused"));
    bool connected = true;
    std::vector<int> writes;
    auto session = [&] { return connected; };
    auto write = [&](int axis) { writes.push_back(axis); return true; };
    assert(WriteMachineSpeed(session, write) == SpeedWriteResult::Ok);
    assert((writes == std::vector<int>{110, 111}));
    for (auto axis : {MachineSpeedAxis::X, MachineSpeedAxis::Y, MachineSpeedAxis::Z}) {
        writes.clear();
        assert(WriteMachineSpeed(session, write, axis) == SpeedWriteResult::Ok);
        assert(writes.size() == 1);
        assert(writes[0] == (axis == MachineSpeedAxis::X ? 110 : axis == MachineSpeedAxis::Y ? 111 : 112));
    }
    writes.clear();
    assert(WriteMachineSpeed(session, write, MachineSpeedAxis::Invalid) == SpeedWriteResult::InvalidAxis);
    assert(writes.empty());
    assert(WriteMachineSpeed(session, [&](int axis) { writes.push_back(axis); connected = false; return true; }) == SpeedWriteResult::SessionLost);
    assert((writes == std::vector<int>{110}));
    connected = true;
    writes.clear();
    assert(WriteMachineSpeed(session, [&](int axis) { writes.push_back(axis); return false; }) == SpeedWriteResult::XFailed);
    assert((writes == std::vector<int>{110}));
    assert(WriteMachineSpeed(session, [](int axis) { return axis == 110; }) == SpeedWriteResult::YFailed);
    assert(WriteMachineSpeed(session, [](int) { return false; }, MachineSpeedAxis::Z) == SpeedWriteResult::ZFailed);
    return 0;
}
''', "speed_transaction")

    def test_readback_never_uses_defaults_or_invalid_data(self):
        compiler = core.find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <limits>
#include "main/boards/lichuang-dev/hutuji_speed_core.h"
int main() {
    using namespace hutuji;
    MachineSpeedSnapshot actual;
    assert(!actual.Complete());
    actual.Observe("110", 10000);
    actual.Observe("111", 9000);
    assert(!SpeedReadbackMatches(MachineSpeedAxis::X, 10000, actual));
    actual.Observe("112", std::numeric_limits<double>::infinity());
    actual.Observe("112", -1);
    assert(actual.revisions[2] == 0);
    actual.Observe("112", 1200);
    assert(actual.Complete());
    assert(SpeedReadbackMatches(MachineSpeedAxis::X, 10000, actual));
    assert(SpeedReadbackMatches(MachineSpeedAxis::Y, 9000, actual));
    assert(SpeedReadbackMatches(MachineSpeedAxis::Z, 1200, actual));
    assert(SpeedReadbackMatches(MachineSpeedAxis::XY, 0, actual));
    assert(!SpeedReadbackMatches(MachineSpeedAxis::XY, 10000, actual));
    actual.Observe("112", std::numeric_limits<double>::quiet_NaN());
    actual.Observe("112", 0);
    actual.Observe("113", 500);
    assert(actual.revisions[2] == 1 && actual.rates[2] == 1200);
    actual.Observe("112", 1200);
    assert(actual.revisions[2] == 2);
    return 0;
}
''', "speed_readback")

    def test_real_worker_single_axis_read_only_and_failure(self):
        compiler = core.find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        source = (core.ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        begin = source.index("void Job::RunSpeedUpdate() {")
        end = source.index("void Job::EnsureJogStepLoaded()", begin)
        worker = source[begin:end]
        prefix = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "main/boards/lichuang-dev/hutuji_speed_core.h"
using TickType_t = unsigned;
unsigned tick = 0;
unsigned xTaskGetTickCount() { return ++tick; }
unsigned pdMS_TO_TICKS(unsigned value) { return value; }
void vTaskDelay(unsigned value) { tick += value; }
constexpr unsigned kJogFreshStateTimeoutMs = 6000;
constexpr unsigned kSpeedWriteTimeoutMs = 5000;
namespace hutuji {
enum class WaitResult { Ok, Failed, Timeout };
enum class GrblState { Idle, Run };
class Pipe {
public:
    bool connected = true;
    unsigned connection = 7, status = 0;
    bool idle = true, shut = false, active = true;
    int stale = -1, mismatch = -1, failed_write = -1, disconnect_after = -1;
    WaitResult reply = WaitResult::Ok;
    std::array<double, 3> physical{10000, 9000, 1200};
    MachineSpeedSnapshot observed;
    std::vector<std::string> lines;
    static Pipe& GetInstance() { static Pipe p; return p; }
    bool IsConnected() const { return connected; }
    bool IsReady() const { return true; }
    bool IsAuthorized() const { return true; }
    bool IsSettingsVerified() const { return true; }
    bool IsNopaperMachine() const { return true; }
    unsigned GetConnectionSequence() const { return connection; }
    unsigned GetResetBannerSequence() const { return 9; }
    unsigned GetStatusReportSequence() const { return status; }
    GrblState GetGrblState() const { return idle ? GrblState::Idle : GrblState::Run; }
    bool SendRealtime(char c) { assert(c == '?'); ++status; return true; }
    bool SendLineForSession(const char* raw, unsigned expected, unsigned banner) {
        assert(expected == 7 && banner == 9);
        if (!connected || connection != expected) return false;
        std::string line(raw);
        lines.push_back(line);
        int setting = std::stoi(line.substr(1));
        int index = setting - 110;
        assert(index >= 0 && index < 3);
        const auto eq = line.find('=');
        reply = WaitResult::Ok;
        if (eq != std::string::npos) {
            if (failed_write == index) reply = WaitResult::Failed;
            else physical[index] = std::stod(line.substr(eq + 1));
        } else if (stale != index) {
            observed.Observe(std::to_string(setting), physical[index] + (mismatch == index ? 1 : 0));
        }
        if (disconnect_after == static_cast<int>(lines.size())) { connected = false; ++connection; }
        return true;
    }
    WaitResult WaitResponse(unsigned) { return reply; }
    MachineSpeedSnapshot GetMachineSpeedSnapshot() const { return observed; }
    void ShutdownSocket(unsigned expected) { assert(expected == 7); shut = true; }
    void SetTaskSessionActive(bool value) { active = value; }
};
class Job {
public:
    std::mutex state_mutex_, stream_mutex_;
    std::string speed_axis_ = "xy", speed_state_ = "pending", speed_reason_;
    int speed_requested_rate_ = 0, speed_applied_rate_ = 0;
    unsigned speed_connection_seq_ = 7;
    unsigned speed_banner_seq_ = 9;
    std::atomic<bool> speed_active_{true}, busy_{true};
    MachineSpeedSnapshot speed_readback_;
    void StartPerformanceHold() {}
    void StopPerformanceHold() {}
    void RunSpeedUpdate();
};
'''
        suffix = r'''
} // namespace hutuji
int main() {
    using namespace hutuji;
    auto& pipe = Pipe::GetInstance();
    {
        Job job;
        job.RunSpeedUpdate();
        assert(job.speed_state_ == "done" && job.speed_applied_rate_ == 0);
        assert((pipe.lines == std::vector<std::string>{"$110", "$111", "$112"}));
        assert((job.speed_readback_.rates == std::array<double, 3>{10000, 9000, 1200}));
        assert(!job.busy_ && !job.speed_active_ && !pipe.active);
    }
    for (int index = 0; index < 3; ++index) {
        pipe = Pipe{};
        Job job;
        job.speed_axis_ = index == 0 ? "x" : index == 1 ? "y" : "z";
        job.speed_requested_rate_ = index == 2 ? 1000 : 8000;
        const auto before = pipe.physical;
        job.RunSpeedUpdate();
        assert(job.speed_state_ == "done" && pipe.lines.size() == 4);
        assert(pipe.lines[0] == "$" + std::to_string(110 + index) + "=" + std::to_string(job.speed_requested_rate_));
        for (int other = 0; other < 3; ++other) {
            assert(pipe.physical[other] == (other == index ? job.speed_requested_rate_ : before[other]));
        }
    }
    {
        pipe = Pipe{};
        Job job;
        job.speed_requested_rate_ = 10000;
        job.RunSpeedUpdate();
        assert(job.speed_state_ == "done" && pipe.lines.size() == 5);
        assert((pipe.physical == std::array<double, 3>{10000, 10000, 1200}));
    }
    for (int failure = 0; failure < 4; ++failure) {
        pipe = Pipe{};
        Job job;
        job.speed_axis_ = "z";
        job.speed_requested_rate_ = 1000;
        if (failure == 0) {
            pipe.stale = 2;
            pipe.observed.Observe("112", 1000); // 即使旧值相同，没有新参数行也必须失败。
        }
        if (failure == 1) pipe.mismatch = 2;
        if (failure == 2) pipe.failed_write = 2;
        if (failure == 3) pipe.disconnect_after = 1;
        job.RunSpeedUpdate();
        assert(job.speed_state_ == "error" && job.speed_applied_rate_ == 0);
        assert(!job.speed_readback_.Complete() && !job.busy_ && !job.speed_active_);
        assert(pipe.shut);
    }
    pipe = Pipe{};
    pipe.idle = false;
    Job blocked;
    blocked.speed_requested_rate_ = 10000;
    blocked.RunSpeedUpdate();
    assert(blocked.speed_state_ == "error" && pipe.lines.empty());
    return 0;
}
'''
        self._compile_and_run(compiler, prefix + worker + suffix, "real_speed_worker")


if __name__ == "__main__":
    unittest.main()
