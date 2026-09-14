"""hutuji_nopaper_core.h 的 host 侧行为钉：VER 机型识别、多页闸、页尾跳过、
$$ 指纹分表（§10.4.15 量产无换纸 SKU）。

与 test_hutuji_recovery_core.py 同法：把纯逻辑编成 host 可执行文件跑行为断言，
非 0 退出即失败。编译器缺失时整个用例类 skip（与仓内既有 core 测试同口径）。
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def find_compiler():
    configured = os.environ.get("CXX", "").strip()
    candidates = [
        Path(configured) if configured else None,
        Path(found) if (found := shutil.which("clang++")) else None,
        Path("C:/Program Files/LLVM/bin/clang++.exe"),
        Path(found) if (found := shutil.which("g++")) else None,
    ]
    mingw_root = Path.home() / "scoop" / "apps" / "mingw"
    if mingw_root.is_dir():
        candidates.extend(sorted(mingw_root.glob("*/bin/g++.exe"), reverse=True))
    return next((path.resolve() for path in candidates if path and path.is_file()), None)


class HutujiNopaperCoreTest(unittest.TestCase):
    def _compile_and_run(self, compiler, source, stem):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            source_path = temp / f"{stem}.cpp"
            output_path = temp / (f"{stem}.exe" if os.name == "nt" else stem)
            source_path.write_text(source, encoding="utf-8")
            build = subprocess.run(
                [
                    str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT), str(source_path), "-o", str(output_path),
                ],
                cwd=ROOT, capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(build.returncode, 0, build.stderr or build.stdout)
            run = subprocess.run([str(output_path)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stderr or run.stdout)

    def test_ver_line_sku_detection(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <string>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"

int main() {
    using hutuji::GrblVerLineIsNopaperSku;
    // 换纸机 / 无换纸机
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.20211103:]"));
    assert(GrblVerLineIsNopaperSku("[VER:1.3a.20260910:]"));
    // build 段精确匹配：前缀/子串陷阱一律不认
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.12026091:]"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.2026091:]"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.202609100:]"));
    // 带尾缀仍取 build 段
    assert(GrblVerLineIsNopaperSku("[VER:1.3a.20260910:custom]"));
    // 非 VER 行 / 残缺行 / 空行：保守不认
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.20260910"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.20260910]"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.]"));
    assert(!GrblVerLineIsNopaperSku("Grbl 1.3a ['$' for help]"));
    assert(!GrblVerLineIsNopaperSku(""));
    return 0;
}
''', "nopaper_ver")

    def test_behaviour_predicates(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <cstring>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"

int main() {
    using namespace hutuji;
    // 页尾跳过：仅无换纸机
    assert(NopaperSkipsPaperChange(true));
    assert(!NopaperSkipsPaperChange(false));
    // 多页闸：无换纸机 pages>1 拒；单页放行；换纸机全放行
    assert(NopaperRejectsMultiPage(true, 2));
    assert(NopaperRejectsMultiPage(true, 20));
    assert(!NopaperRejectsMultiPage(true, 1));
    assert(!NopaperRejectsMultiPage(true, 0));
    assert(!NopaperRejectsMultiPage(false, 20));
    // 拒绝话术字面量（与 §10.4.15 一致，改文案须同步契约）
    assert(std::strcmp(kNopaperMultiPageRejectMsg,
                       "这台机器一次只能画一页哦") == 0);
    return 0;
}
''', "nopaper_predicates")

    def test_golden_table_split(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <cstring>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"

int main() {
    using namespace hutuji;
    const GrblSettingGolden* table = nullptr;
    size_t count = 0;
    // 换纸机：13 项含长名 Verbose 锁表项
    ActiveGrblSettingGoldens(false, table, count);
    assert(count == kGrblSettingGoldenCount);
    bool has_verbose = false;
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(table[i].response_key, "Errors/Verbose") == 0) has_verbose = true;
    }
    assert(has_verbose);
    // 无换纸机：10 项、无长名项；$110/$111 速度项已从金表删除（2026-09-12
    // 金表瘦身：速度下放为 hutuji.speed 工具自由调整，探活只锁安全项，
    // 速度任意值不再触发 grbl_settings_mismatch 拒画）。
    ActiveGrblSettingGoldens(true, table, count);
    assert(count == kGrblSettingGoldenNopaperCount);
    assert(count == 10);
    for (size_t i = 0; i < count; ++i) {
        assert(std::strcmp(table[i].response_key, "Errors/Verbose") != 0);
    }
    assert(table[0].expected == 255.0 && table[0].integer);   // $1 弹簧笔常使能
    assert(table[1].expected == 4.0 && table[1].integer);     // $3 只反 Z（bit Z=4）
    assert(table[7].expected == 200.0);                       // $130 行程
    assert(table[8].expected == 200.0);                       // $131 行程
    assert(table[9].expected == 20.0);                        // $132 笔程
    for (size_t i = 0; i < count; ++i) {
        assert(std::strcmp(table[i].response_key, "110") != 0 &&
               std::strcmp(table[i].response_key, "111") != 0);  // 速度项不得回流
    }
    return 0;
}
''', "nopaper_goldens")


    def test_motor_release_requires_fresh_idle_on_the_same_verified_machine(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r"""
#include <cassert>
#include <cstdint>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"

int main() {
    using namespace hutuji;
    const IdleMotorReleaseSnapshot valid{true, true, true, true, true, true, true, 8, 3, 41};
    assert(CanReleaseNopaperMotors(valid, 8, 3, 40));
    for (int field = 0; field < 7; ++field) {
        auto changed = valid;
        bool* gates[] = {&changed.connected, &changed.ready, &changed.authorized,
                        &changed.settings_verified, &changed.nopaper, &changed.idle,
                        &changed.line_mode};
        *gates[field] = false;
        assert(!CanReleaseNopaperMotors(changed, 8, 3, 40));
    }
    assert(!CanReleaseNopaperMotors(valid, 9, 3, 40));
    assert(!CanReleaseNopaperMotors(valid, 8, 4, 40));
    assert(!CanReleaseNopaperMotors(valid, 8, 3, 41));
    auto wrapped = valid;
    wrapped.status = 0;
    assert(CanReleaseNopaperMotors(wrapped, 8, 3, UINT32_MAX));
    return 0;
}
""", "nopaper_motor_release")

    def test_release_follows_confirmed_home_and_never_the_page_continue(self):
        # 机械收尾接线回归：不是把任意 Idle 当作任务完成，也不能在下一页前释放。
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        run = source[source.index("void Job::Run() {"):source.index("Http* Job::AcquireFetchClient()")]
        release = run.index("ok = ReleaseMotorsAfterHome(")
        self.assertLess(run.index("ok = ReturnHomeAfterDraw();"), release)
        self.assertIn("if (ok && !more_pages && nopaper_end)", run[:release])
        self.assertLess(release, run.index('SetState("done")'))
        # 多页后验拒绝必须先改变 ok，再进入 error/done 分支；拒绝不能落入 done。
        reject = run.index("last_error_ = hutuji::kNopaperMultiPageRejectMsg;")
        self.assertLess(reject, run.index("} else if (ok)", reject))
        for begin, end in (("bool Job::PerformAbortDrainHome()", "bool Job::WaitForAbortReset()"),
                           ("bool Job::HomeAfterAbort(", "bool Job::ChangePaperAfterDraw()")):
            body = source[source.index(begin):source.index(end)]
            self.assertLess(body.index("WaitForIdle(false, kHomeIdleTimeoutMs)"),
                            body.index("ReleaseMotorsAfterHome("))

    def test_release_commit_excludes_late_controls_and_does_not_leak_ack(self):
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        for begin, end in (("std::string Job::RequestAbort()", "bool Job::StartAbortResetTask()"),
                           ("std::string Job::RequestPause()", "std::string Job::RequestResume()"),
                           ("std::string Job::RequestResume()", "std::string Job::RequestRepeat()")):
            body = source[source.index(begin):source.index(end)]
            self.assertIn("finishing_at_home_", body)
        release = source[source.index("bool Job::ReleaseMotorsAfterHome("):]
        self.assertLess(release.index("WaitForIdle(honor_abort"), release.index("finishing_at_home_ = true"))
        self.assertLess(release.index("finishing_at_home_ = true"), release.index("SendMotorDisableAtIdle("))
        self.assertIn("pipe.ShutdownSocket(connection);", release)
        pipe = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(encoding="utf-8")
        body = pipe[pipe.index("bool Pipe::SendMotorDisableAtIdle("):pipe.index("bool Pipe::HasFreshStoppedStatus(")]
        self.assertLess(body.index("lock(write_mutex_)"), body.index("CanReleaseNopaperMotors("))
        self.assertLess(body.index("CanReleaseNopaperMotors("), body.index("SendRawLocked("))
        self.assertIn("kMotorDisableLine", body)
        self.assertNotIn("$SLP", body)
        self.assertNotIn("$1=", body)

    def test_repeated_abort_reuses_the_actual_worker_until_job_cleanup(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        request = source[source.index("std::string Job::RequestAbort()"):
                         source.index("bool Job::StartAbortResetTask()")]
        start_begin = source.index("bool Job::StartAbortDrainHomeTask()")
        start = source[start_begin:source.index("bool Job::PerformAbortDrainHome()", start_begin)]
        # 编译产品的真实入口和任务创建函数；只替换 RTOS 调度与 UI，保持 worker 未执行，
        # 模拟用户在归位期间连点停止，以及 worker 已结束但 Job 尚未释放 busy 的窗口。
        self._compile_and_run(compiler, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
using BaseType_t = int;
constexpr int pdPASS = 1;
int create_calls = 0;
bool creation_succeeds = true;
void (*pending)(void*) = nullptr;
int xTaskCreate(void (*entry)(void*), const char*, int, void*, int, void*) {
    ++create_calls;
    if (creation_succeeds) pending = entry;
    return creation_succeeds ? pdPASS : 0;
}
void vTaskDelete(void*) {}
std::string JsonString(const char* text) { return text; }
uint32_t NextStreamControlEpoch(uint32_t value) { return value + 1; }
struct Job {
    std::mutex stream_mutex_, state_mutex_;
    std::atomic<bool> busy_{true}, speed_active_{false}, awaiting_confirmation_{false};
    std::atomic<bool> paper_update_active_{false};
    std::atomic<bool> prefetch_cancel_{false}, abort_requested_{false};
    std::atomic<bool> pen_test_active_{false}, paper_active_{false};
    std::atomic<bool> abort_reset_worker_active_{false};
    std::atomic<uint32_t> stream_control_epoch_{0};
    bool finishing_at_home_ = false;
    std::string state_ = "streaming";
    static Job& GetInstance() { static Job instance; return instance; }
    void ClearPreview() {}
    void Notify(const char*) {}
    void SetState(const char* text) { state_ = text; }
    bool PerformAbortDrainHome() { return false; }
    std::string RequestAbort();
    bool StartAbortDrainHomeTask();
};
''' + request + start + r'''
int main() {
    auto& job = Job::GetInstance();
    job.paper_update_active_.store(true);
    assert(job.RequestAbort().find("error") != std::string::npos);
    assert(create_calls == 0 && !job.abort_requested_.load());
    job.paper_update_active_.store(false);
    job.RequestAbort();
    assert(create_calls == 1 && job.abort_reset_worker_active_.load());
    const auto epoch = job.stream_control_epoch_.load();
    for (int i = 0; i < 20; ++i) job.RequestAbort();
    assert(create_calls == 1);
    assert(job.stream_control_epoch_.load() == epoch);
    assert(job.StartAbortDrainHomeTask());
    assert(create_calls == 1);  // 任务创建面也拒重复占有者。
    pending(nullptr);
    assert(!job.abort_reset_worker_active_.load());
    job.RequestAbort();
    assert(create_calls == 1);  // 已归位，Job 尚未清 busy 也不再启动。
    job.busy_.store(false);
    job.RequestAbort();
    assert(create_calls == 1);
    // 新任务与任务创建失败后的重试都能重新取得 worker。
    job.busy_.store(true);
    job.abort_requested_.store(false);
    creation_succeeds = false;
    assert(job.RequestAbort().find("error") != std::string::npos);
    assert(!job.abort_requested_.load() && !job.abort_reset_worker_active_.load());
    assert(create_calls == 2);
    creation_succeeds = true;
    job.RequestAbort();
    assert(create_calls == 3 && job.abort_reset_worker_active_.load());
    pending(nullptr);
    return 0;
}
''', "abort_worker_idempotence")

    def test_abort_terminal_is_published_after_the_worker_settles(self):
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        tail = source[source.index("const bool nopaper_end ="):
                      source.index("Http* Job::AcquireFetchClient()")]
        waited = tail.index("const bool waited = WaitForAbortReset();")
        terminal = tail.index('SetState("aborted");')
        self.assertGreater(terminal, waited)
        self.assertLess(terminal, tail.index("busy_.store(false, std::memory_order_release);"))


if __name__ == "__main__":
    unittest.main()
