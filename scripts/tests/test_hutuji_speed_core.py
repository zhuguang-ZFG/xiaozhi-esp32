"""速度范围、会话切换与部分失败不能被报成两轴成功。"""
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
    assert(IsSpeedSettledState("done") && !IsSpeedSettledState("paused"));
    bool connected = true;
    std::vector<int> writes;
    auto session = [&] { return connected; };
    assert(WriteMachineSpeed(session, [&](int axis) { writes.push_back(axis); return true; }) == SpeedWriteResult::Ok);
    assert((writes == std::vector<int>{110, 111}));
    writes.clear();
    assert(WriteMachineSpeed(session, [&](int axis) { writes.push_back(axis); connected = false; return true; }) == SpeedWriteResult::SessionLost);
    assert((writes == std::vector<int>{110}));
    connected = true;
    writes.clear();
    assert(WriteMachineSpeed(session, [&](int axis) { writes.push_back(axis); return false; }) == SpeedWriteResult::XFailed);
    assert((writes == std::vector<int>{110}));
    assert(WriteMachineSpeed(session, [](int axis) { return axis == 110; }) == SpeedWriteResult::YFailed);
    return 0;
}
''', "speed_transaction")


if __name__ == "__main__":
    unittest.main()
