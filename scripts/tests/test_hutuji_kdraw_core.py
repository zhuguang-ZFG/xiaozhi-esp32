"""真实编译 UDP 完成判定：失联、重复、乱序、异机与换纸不产生完成。"""
import unittest
import test_hutuji_nopaper_core as core


class HutujiKdrawCoreTest(unittest.TestCase):
    _compile_and_run = core.HutujiNopaperCoreTest._compile_and_run

    def test_beacon_completion(self):
        compiler = core.find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include "main/boards/lichuang-dev/hutuji_kdraw_core.h"
using namespace hutuji::kdraw;
int main() {
    Beacon b;
    assert(ParseBeacon("<Run|Changing=Off|Seq=1>", b));
    assert(!ParseBeacon("<Idle|Seq=2>", b));
    assert(!ParseBeacon("<Idle|Changing=bad|Seq=2>", b));
    assert(!ParseBeacon("<Idle|Changing=Off|Seq=2x>", b));
    assert(!ParseBeacon("<Idle|Changing=Off|Seq=4294967296>", b));
    assert(!ParseBeacon("<Idle|Changing=Off|Seq=2|Seq=3>", b));
    assert(ParseBeacon("<Idle|Seq=4294967295|Changing=Off>", b));
    auto send = [](CompletionTracker& t, BeaconState state, uint32_t seq, uint64_t now,
                   bool changing = false, uint32_t source = 7, uint32_t expected = 7) {
        return t.Observe(source, expected, Beacon{state, changing, seq}, now);
    };
    CompletionTracker t;
    assert(!send(t, BeaconState::Run, 1, 0));
    assert(!send(t, BeaconState::Idle, 2, 100));
    t.Expire(3000);  // 一帧 Idle 后失联，后续 Idle 也不能替历史任务庆祝。
    assert(!send(t, BeaconState::Idle, 3, 3100));
    assert(!send(t, BeaconState::Idle, 4, 4100));
    assert(!send(t, BeaconState::Idle, 5, 5100));
    assert(!send(t, BeaconState::Idle, 6, 6100));
    t = CompletionTracker{};
    assert(!send(t, BeaconState::Run, 1, 0));
    assert(!send(t, BeaconState::Idle, 2, 100));
    assert(!send(t, BeaconState::Idle, 2, 1100));  // 重复包不刷新失联时间。
    assert(!send(t, BeaconState::Idle, 1, 2100));
    assert(!send(t, BeaconState::Idle, 3, 2700));
    t = CompletionTracker{};
    assert(!send(t, BeaconState::Run, 1, 0, false, 8));
    assert(!send(t, BeaconState::Idle, 2, 100));
    assert(!send(t, BeaconState::Idle, 3, 1100));
    assert(!send(t, BeaconState::Idle, 4, 2100));
    assert(!send(t, BeaconState::Idle, 5, 3100));
    t = CompletionTracker{};
    assert(!send(t, BeaconState::Run, 1, 0));
    assert(!send(t, BeaconState::Idle, 2, 100));
    assert(!send(t, BeaconState::Idle, 3, 1100, true));
    assert(!send(t, BeaconState::Idle, 4, 2100));
    assert(!send(t, BeaconState::Idle, 5, 3100));
    assert(!send(t, BeaconState::Idle, 6, 4100));
    assert(send(t, BeaconState::Idle, 7, 5100));
    assert(!send(t, BeaconState::Idle, 8, 6100));
    // uint32 序号正常回绕与下一轮任务。
    t = CompletionTracker{};
    assert(!send(t, BeaconState::Run, 0xfffffffeu, 0));
    assert(!send(t, BeaconState::Idle, 0xffffffffu, 100));
    assert(!send(t, BeaconState::Idle, 0, 1100));
    assert(!send(t, BeaconState::Idle, 1, 2100));
    assert(send(t, BeaconState::Idle, 2, 3100));
    assert(!send(t, BeaconState::Run, 3, 4100));
    assert(!send(t, BeaconState::Interrupted, 4, 4200));
    assert(!send(t, BeaconState::Idle, 5, 4300));
    assert(!send(t, BeaconState::Idle, 6, 5300));
    assert(!send(t, BeaconState::Idle, 7, 6300));
    assert(!send(t, BeaconState::Idle, 8, 7300));
    return 0;
}
''', "beacon_completion")


if __name__ == "__main__":
    unittest.main()
