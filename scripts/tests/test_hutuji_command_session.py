"""在取得写锁时注入会话变化，验证真实命令入口不会写到新连接。"""
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


class CommandSessionTest(unittest.TestCase):
    def test_session_change_while_waiting_for_writer_sends_no_bytes(self):
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('需要 host C++ 编译器')
        source = (ROOT / 'main/boards/lichuang-dev/hutuji_pipe.cc').read_text(encoding='utf-8')
        functions = '\n'.join(function_body(source, signature) for signature in (
            'bool Pipe::SendLine(', 'bool Pipe::SendLineForSession(', 'bool Pipe::SendManualReset(',
            'bool Pipe::IsCommandSessionCurrent(', 'bool Pipe::SendLineLocked('))
        # 仅把平台互斥量替换成可观察的同义锁，在进入 lock 后精确插入连接变化。
        functions = functions.replace('std::mutex', 'ObservedMutex')
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define HUTUJI_QUIET_STREAM_LOG 1
using TickType_t = uint32_t;
constexpr uint32_t portTICK_PERIOD_MS = 1;
TickType_t xTaskGetTickCount() {
    return static_cast<TickType_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool IsStreamingMotionLine(const std::string&) { return false; }
struct ObservedMutex {
    std::mutex value;
    std::atomic<bool> entered{false};
    void lock() { entered.store(true); value.lock(); }
    void unlock() { value.unlock(); }
};
class Pipe {
public:
    ObservedMutex write_mutex_;
    std::atomic<bool> connected_{true}, ready_{true}, authorized_{true}, settings_verified_{true};
    std::atomic<bool> task_session_active_{true}, drain_on_send_{true};
    std::atomic<uint32_t> connection_seq_{3}, reset_banner_seq_{5};
    std::vector<std::string> sent;
    int drains = 0;
    void DrainResponses() { ++drains; }
    bool SendRawLocked(const char* bytes, size_t length) { sent.emplace_back(bytes, length); return true; }
    bool SendLine(const std::string& line);
    bool SendLineForSession(const std::string& line, uint32_t connection, uint32_t banner);
    bool SendManualReset(uint32_t connection, uint32_t banner);
    bool IsCommandSessionCurrent(uint32_t connection, uint32_t banner) const;
    bool SendLineLocked(const std::string& line);
};
''' + functions + r'''
int main() {
    for (bool reset : {false, true}) {
        for (int scenario = 0; scenario < 6; ++scenario) {
            Pipe pipe;
            pipe.write_mutex_.value.lock();
            bool result = false;
            std::thread worker([&]() {
                result = reset ? pipe.SendManualReset(3, 5) : pipe.SendLineForSession("G1 Z0", 3, 5);
            });
            while (!pipe.write_mutex_.entered.load()) std::this_thread::yield();
            if (scenario == 1) ++pipe.connection_seq_;
            if (scenario == 2) ++pipe.reset_banner_seq_;
            if (scenario == 3) pipe.ready_.store(false);
            if (scenario == 4) pipe.task_session_active_.store(false);
            if (scenario == 5) pipe.authorized_.store(false);
            pipe.write_mutex_.value.unlock();
            worker.join();
            assert(result == (scenario == 0));
            assert(pipe.sent.size() == (scenario == 0 ? 1U : 0U));
            if (scenario == 0) {
                assert(pipe.sent[0] == (reset ? std::string(1, 0x18) : "G1 Z0\n"));
                assert(pipe.drains == (reset ? 0 : 1));
            } else assert(pipe.drains == 0);
        }
    }
    Pipe probe;
    probe.ready_.store(false);
    probe.authorized_.store(false);
    probe.task_session_active_.store(false);
    assert(probe.SendLine("$I"));
    assert(probe.sent[0] == "$I\n");
    probe.drain_on_send_.store(false);
    assert(probe.SendLine("G1 X1"));
    assert(probe.drains == 1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            stem = Path(directory) / 'command_session'
            stem.with_suffix('.cpp').write_text(program, encoding='utf-8')
            exe = stem.with_suffix('.exe' if os.name == 'nt' else '')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                                     str(stem.with_suffix('.cpp')), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr or result.stdout)


if __name__ == '__main__':
    unittest.main()
