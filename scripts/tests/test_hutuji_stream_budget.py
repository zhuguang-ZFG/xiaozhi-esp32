"""混合行长与延迟ACK下，真实流控决策不得耗尽16段TCP队列或破坏字节窗。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StreamBudgetTest(unittest.TestCase):
    def test_delayed_ack_and_mixed_line_lengths(self):
        compiler = os.environ.get('CXX') or shutil.which('g++')
        if not compiler: self.skipTest('需要host C++编译器')
        program = r'''
#include "hutuji_recovery_core.h"
#include <cassert>
#include <deque>
#include <limits>
int main() {
    using namespace hutuji;
    // 复现旧字节窗：16个26字节小包仍被许可继续灌，TCP已无段额度。
    assert(16 * 26 + 26 < kStreamWindowBytes);
    assert(!StreamWindowAllowsSend(16 * 26, 16, 26));
    assert(!StreamWindowAllowsSend(8 * 2, 8, 2));
    assert(StreamWindowAllowsSend(7 * 26, 7, 26));
    assert(StreamWindowAllowsSend(255, 1, 255));
    assert(!StreamWindowAllowsSend(256, 1, 256));
    assert(!StreamWindowAllowsSend(std::numeric_limits<size_t>::max(), 1, 2));
    assert(StreamWindowAllowsSend(0, 0, 255));
    const size_t lengths[] = {2, 26, 26, 255, 26, 80, 26, 255, 2};
    std::deque<size_t> pending;
    size_t bytes = 0, sent = 0, acked = 0;
    while (acked < 1000) {
        while (sent < 1000 && StreamWindowAllowsSend(bytes, pending.size(), lengths[sent % 9])) {
            pending.push_back(lengths[sent++ % 9]);
            bytes += pending.back();
            assert(bytes < 512 && pending.size() < 16);
        }
        assert(!pending.empty());
        // 每拍只释放最早应答，下一行只有真实ACK后才可补入。
        bytes -= pending.front();
        pending.pop_front();
        ++acked;
    }
    assert(sent == 1000 && pending.empty() && bytes == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'budget.cpp').write_text(program, encoding='utf-8')
            exe = path / 'budget.exe'
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(ROOT / 'main/boards/lichuang-dev'), str(path / 'budget.cpp'), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr)
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            run = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)
        job = (ROOT / 'main/boards/lichuang-dev/hutuji_job.cc').read_text(encoding='utf-8')
        self.assertIn('StreamWindowAllowsSend(c_line_bytes_sum, c_line.size(), need)', job)


if __name__ == '__main__':
    unittest.main()
