"""编译真实 Job 状态等待，注入重连、复位、停止和迟到状态报告。"""
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


class FreshSessionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('需要 host C++ 编译器')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        stem = Path(cls.directory.name) / 'fresh_session'
        source = (ROOT / 'main/boards/lichuang-dev/hutuji_job.cc').read_text(encoding='utf-8')
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
using TickType_t = uint32_t;
#define pdMS_TO_TICKS(ms) (ms)
TickType_t now = 0;
std::function<void()> on_delay;
TickType_t xTaskGetTickCount() { return now; }
void vTaskDelay(TickType_t duration) {
    now += duration;
    if (on_delay) on_delay();
    assert(now < 10000);
}
enum class GrblState { Unknown, Idle, Run, Alarm };
class Pipe {
public:
    bool connected = true, ready = true;
    uint32_t connection = 3, banner = 5, status = 11, mpos = 7;
    GrblState state = GrblState::Run;
    static Pipe& GetInstance() { static Pipe instance; return instance; }
    bool IsConnected() const { return connected; }
    bool IsReady() const { return ready; }
    uint32_t GetConnectionSequence() const { return connection; }
    uint32_t GetResetBannerSequence() const { return banner; }
    uint32_t GetStatusReportSequence() const { return status; }
    uint32_t GetMposReportSequence() const { return mpos; }
    GrblState GetGrblState() const { return state; }
    int GetAlarmCode() const { return 1; }
    bool SendRealtime(uint8_t value) { assert(value == '?'); return connected; }
};
class Job {
public:
    std::atomic<bool> abort_requested_{false};
    uint32_t stream_connection_seq_ = 3;
    bool stream_disconnected_ = false;
    std::string last_error_;
    bool WaitWhilePaused() { return !abort_requested_.load(); }
    bool QueryAndWaitFreshMachineState(uint32_t timeout_ms);
    bool WaitForIdle(bool honor_abort, uint32_t timeout_ms);
};
''' + function_body(source, 'bool Job::QueryAndWaitFreshMachineState(') + '\n' + function_body(source, 'bool Job::WaitForIdle(') + r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    const int scenario = std::atoi(argv[1]);
    Job job;
    auto& pipe = Pipe::GetInstance();
    bool once = false;
    on_delay = [&]() {
        if (once) return;
        once = true;
        if (scenario == 2) { ++pipe.status; return; }
        if (scenario == 3) return;
        if (scenario == 4 || scenario == 8 || scenario == 9) ++pipe.connection;
        if (scenario == 5 || scenario == 10) { ++pipe.banner; pipe.ready = false; }
        if (scenario == 6 || scenario == 11) pipe.connected = false;
        if (scenario == 12 || scenario == 13) job.abort_requested_.store(true);
        ++pipe.status;
        ++pipe.mpos;
        pipe.state = GrblState::Idle;
    };
    if (scenario <= 6) {
        const bool result = job.QueryAndWaitFreshMachineState(250);
        assert(result == (scenario == 0 || scenario == 1));
        if (scenario >= 4) assert(now <= 100);
    } else {
        const bool honor_abort = scenario != 8 && scenario != 10 && scenario != 13;
        const bool result = job.WaitForIdle(honor_abort, 500);
        assert(result == (scenario == 7 || scenario == 13));
        if (scenario != 7 && scenario != 13) assert(!job.last_error_.empty());
        if (scenario == 9 || scenario == 11) assert(job.stream_disconnected_);
    }
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

    def test_query_fresh_status_and_position(self): self.run_case(1)
    def test_query_rejects_status_without_position(self): self.run_case(2)
    def test_query_times_out_without_report(self): self.run_case(3)
    def test_query_rejects_reconnect_report(self): self.run_case(4)
    def test_query_rejects_reset_report(self): self.run_case(5)
    def test_query_rejects_disconnect_with_late_report(self): self.run_case(6)
    def test_idle_requires_fresh_report(self): self.run_case(7)
    def test_manual_idle_rejects_reconnect(self): self.run_case(8)
    def test_draw_idle_rejects_reconnect(self): self.run_case(9)
    def test_manual_idle_rejects_reset(self): self.run_case(10)
    def test_draw_idle_rejects_disconnect(self): self.run_case(11)
    def test_draw_idle_does_not_override_abort(self): self.run_case(12)
    def test_recovery_idle_may_ignore_abort_same_session(self): self.run_case(13)


if __name__ == '__main__':
    unittest.main()
