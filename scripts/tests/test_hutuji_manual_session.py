"""执行真实手动控制 worker：拒绝跨连接应答，失败必须能从状态看见。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get('XIAOZHI_ROOT', Path(__file__).resolve().parents[2]))


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError('函数体未闭合')


class ManualSessionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('需要 host C++ 编译器')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        stem = Path(cls.directory.name) / 'manual_session'
        source = (ROOT / 'main/boards/lichuang-dev/hutuji_job.cc').read_text(encoding='utf-8')
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
using TickType_t = uint32_t;
#define pdMS_TO_TICKS(ms) (ms)
TickType_t now = 0;
int scenario = 0;
TickType_t xTaskGetTickCount() { return now; }
void vTaskDelay(TickType_t duration) { now += duration; assert(now < 15000); }
const uint32_t kHomeOkTimeoutMs = 500;
const uint32_t kHomeIdleTimeoutMs = 500;
const uint32_t kPenOriginIdleTimeoutMs = 500;
const uint32_t kJogFreshStateTimeoutMs = 500;
enum class WaitResult { Ok, Timeout, Failed };
namespace hutuji {
const char* kMotorDisableLine = "$MD";
enum class JogVerdict { kOk };
void MachineJogEnvelope(bool, float&, float&) {}
JogVerdict DecideJog(float, float, float, float, float, float) { return JogVerdict::kOk; }
}
class Pipe {
public:
    bool connected = true, ready = true, authorized = true, settings = true, task_active = true;
    uint32_t connection = 3, banner = 5;
    int shutdowns = 0;
    std::vector<std::string> sent;
    static Pipe& GetInstance() { static Pipe instance; return instance; }
    bool IsConnected() const { return connected; }
    bool IsReady() const { return ready; }
    bool IsAuthorized() const { return authorized; }
    bool IsSettingsVerified() const { return settings; }
    bool IsNopaperMachine() const { return true; }
    uint32_t GetConnectionSequence() const { return connection; }
    uint32_t GetResetBannerSequence() const { return banner; }
    bool SendLine(const char* line) { sent.emplace_back(line); return true; }
    bool SendLineForSession(const char* line, uint32_t expected, uint32_t reset) {
        if (connection != expected || banner != reset || !ready) return false;
        return SendLine(line);
    }
    WaitResult WaitResponse(uint32_t timeout, std::string* = nullptr, int* error = nullptr) {
        now += timeout;
        if (error) *error = -1;
        if (scenario == 2) ++connection;
        if (scenario == 3) { ++banner; ready = false; }
        if (scenario == 4) return WaitResult::Timeout;
        if (scenario == 5) { if (error) *error = 9; return WaitResult::Failed; }
        return WaitResult::Ok;
    }
    bool SendRealtime(uint8_t value) {
        assert(value == 0x18);
        if (scenario == 9) { ++banner; ready = false; authorized = false; }
        return true;
    }
    bool SendManualReset(uint32_t expected, uint32_t reset) {
        if (connection != expected || banner != reset || !ready) return false;
        return SendRealtime(0x18);
    }
    void SetTaskSessionActive(bool value) { task_active = value; }
    void ShutdownSocket(uint32_t expected) {
        if (expected == connection) { ++shutdowns; connected = false; }
    }
    void GetMachinePos(float& x, float& y, float& z) { x = y = z = 10; }
};
class Job {
public:
    std::string pending_manual_action_ = "pen_up";
    uint32_t manual_connection_seq_ = 3, manual_banner_seq_ = 5;
    std::mutex stream_mutex_, state_mutex_;
    std::atomic<bool> busy_{true}, manual_pen_down_latched_{false};
    std::string state_ = "manual", last_error_, manual_error_, notification;
    float GetJogStepMm() { return 10; }
    bool GetPaperJogEnvelope(float& x, float& y) { x=205; y=290; return true; }
    bool QueryAndWaitFreshMachineState(uint32_t) { return true; }
    bool WaitForIdle(bool, uint32_t) {
        if (scenario == 6) { last_error_ = "等待写字机运动完成超时"; return false; }
        if (scenario == 12) ++Pipe::GetInstance().connection;
        return true;
    }
    void StartPerformanceHold() {}
    void StopPerformanceHold() {}
    void SetState(const char* value) { state_ = value; }
    void Notify(const std::string& value) { notification = value; }
    void ManualTask();
};
''' + function_body(source, 'void Job::ManualTask()') + r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    scenario = std::atoi(argv[1]);
    Job job;
    auto& pipe = Pipe::GetInstance();
    if (scenario == 1) ++pipe.connection;
    if (scenario == 2 || scenario == 3) job.pending_manual_action_ = "pen_down";
    if (scenario == 7) pipe.authorized = false;
    if (scenario == 8) job.pending_manual_action_ = "set_origin";
    if (scenario == 9 || scenario == 10) job.pending_manual_action_ = "reset";
    if (scenario == 11) { ++pipe.banner; pipe.ready = false; }
    if (scenario == 13) pipe.ready = false;
    job.ManualTask();
    const bool expected_success = scenario == 0 || scenario == 8 || scenario == 9;
    assert(job.state_ == (expected_success ? "idle" : "error"));
    assert(!job.busy_.load());
    assert(!pipe.task_active);
    if (!expected_success) assert(!job.manual_error_.empty());
    if (scenario == 1 || scenario == 7 || scenario == 11 || scenario == 13) assert(pipe.sent.empty());
    if (scenario == 2 || scenario == 3) assert(pipe.sent.size() == 1);
    if (scenario == 4 || scenario == 5 || scenario == 6 || scenario == 10) assert(pipe.shutdowns == 1);
    if (scenario == 1 || scenario == 2 || scenario == 12) assert(pipe.shutdowns == 0);
    if (scenario == 0) assert(pipe.sent.size() == 1 && pipe.sent[0] == "G1G90 Z0.0F10000");
    if (scenario == 8) assert(pipe.sent.size() == 1 && pipe.sent[0] == "G92 X0.0 Y0.0 Z0");
    assert(!job.manual_pen_down_latched_.load());
}
'''
        stem.with_suffix('.cpp').write_text(program, encoding='utf-8')
        cls.exe = stem.with_suffix('.exe' if os.name == 'nt' else '')
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                 str(stem.with_suffix('.cpp')), '-o', str(cls.exe)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stderr or result.stdout)
        cls.env = dict(os.environ)
        cls.env['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.env.get('PATH', '')

    def run_case(self, scenario):
        result = subprocess.run([str(self.exe), str(scenario)], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)

    def test_success_releases_session(self): self.run_case(0)
    def test_worker_rejects_reconnection_before_execution(self): self.run_case(1)
    def test_pen_down_stops_before_second_line_after_reconnect(self): self.run_case(2)
    def test_pen_down_stops_after_reset_banner(self): self.run_case(3)
    def test_reply_timeout_closes_only_original_session(self): self.run_case(4)
    def test_reply_error_is_visible(self): self.run_case(5)
    def test_idle_failure_is_visible(self): self.run_case(6)
    def test_lost_authorization_prevents_send(self): self.run_case(7)
    def test_set_origin_still_uses_same_command(self): self.run_case(8)
    def test_requested_reset_accepts_its_own_banner(self): self.run_case(9)
    def test_reset_timeout_is_visible(self): self.run_case(10)
    def test_worker_rejects_reset_before_execution(self): self.run_case(11)
    def test_late_idle_does_not_complete_new_session(self): self.run_case(12)
    def test_unready_worker_sends_nothing(self): self.run_case(13)


if __name__ == '__main__':
    unittest.main()
