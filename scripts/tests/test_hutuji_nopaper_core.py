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
            wait = "WaitForIdleForSession(false, kHomeIdleTimeoutMs" if "Drain" in begin else "WaitForIdle(false, kHomeIdleTimeoutMs)"
            self.assertLess(body.index(wait),
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

    def _compile_abort_harness(self, main, stem):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        request = source[source.index("std::string Job::RequestAbort()"):
                         source.index("bool Job::StartAbortResetTask()")]
        start_begin = source.index("bool Job::StartAbortDrainHomeTask()")
        start = source[start_begin:source.index("bool Job::PerformAbortDrainHome()", start_begin)]
        wait = source[source.index("bool Job::WaitForAbortReset()"):
                      source.index("std::string Job::RequestPause()")]
        run = source[source.index("void Job::Run() {"):
                     source.index("Http* Job::AcquireFetchClient()")]
        tail = "void Job::RunTail(bool ok) {\n" + run[run.index("    bool reset_ok = true;"):]
        # 解锁钩子在旧窗口内执行真实 Run 尾段。
        product = (request + start + wait + tail).replace("std::mutex", "HookMutex")
        self._compile_and_run(compiler, r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include "main/boards/lichuang-dev/hutuji_recovery_core.h"
using hutuji::AbortResetOwnerPhase;
using hutuji::AbortDrainPhase;
using hutuji::NextStreamControlEpoch;
using BaseType_t = int;
using TickType_t = uint32_t;
constexpr int pdPASS = 1;
constexpr uint32_t kResetRecoveryTimeoutMs = 30000;
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
struct HookMutex {
    std::mutex mutex;
    std::function<void()> on_unlock;
    void lock() { mutex.lock(); }
    bool try_lock() { return mutex.try_lock(); }
    void unlock() {
        mutex.unlock();
        if (auto hook = std::exchange(on_unlock, {})) hook();
    }
};
int create_calls = 0, wait_calls = 0, home_calls = 0, delete_calls = 0;
bool creation_succeeds = true;
void (*pending)(void*) = nullptr;
int xTaskCreate(void (*entry)(void*), const char*, int, void*, int, void*) {
    ++create_calls;
    if (creation_succeeds) {
        assert(pending == nullptr);
        pending = entry;
    }
    return creation_succeeds ? pdPASS : 0;
}
void RunQueuedWorker();
TickType_t ticks = 0;
TickType_t xTaskGetTickCount() { return ticks; }
void vTaskDelay(TickType_t delay) {
    ticks += delay;
    ++wait_calls;
    RunQueuedWorker();
}
void vTaskDelete(void*) { ++delete_calls; }
std::string JsonString(const char* text) { return text; }
enum class GrblState { Idle };
struct Pipe {
    static Pipe& GetInstance() { static Pipe instance; return instance; }
    bool IsConnected() { return true; }
    GrblState GetGrblState() { return GrblState::Idle; }
    void SetExpectBlockingPeer(bool) {}
    void SetTaskSessionActive(bool) {}
    void ShutdownSocket(uint32_t) { assert(false && "unexpected wait timeout"); }
};
struct Display { void SetStatus(const char*) {} };
struct Board {
    static Board& GetInstance() { static Board instance; return instance; }
    Display* GetDisplay() { return nullptr; }
};
struct Job {
    HookMutex stream_mutex_, state_mutex_;
    std::atomic<bool> busy_{true}, speed_active_{false}, awaiting_confirmation_{false};
    std::atomic<bool> paper_update_active_{false}, ota_reserved_{false};
    std::atomic<bool> prefetch_cancel_{false}, abort_requested_{false};
    std::atomic<bool> pen_test_active_{false}, paper_active_{false};
    std::atomic<bool> abort_reset_worker_active_{false}, abort_home_done_{false};
    std::atomic<bool> paused_{false}, repeat_mode_{false}, buffer_replayable_{false};
    std::atomic<uint32_t> stream_control_epoch_{0}, abort_reset_session_{0};
    hutuji::AbortResetOwner abort_reset_owner_;
    std::atomic<AbortDrainPhase> abort_drain_phase_{AbortDrainPhase::Idle};
    bool finishing_at_home_ = false;
    char* buffer_ = nullptr;
    int preview_clears = 0;
    std::string state_ = "streaming", last_error_;
    static Job& GetInstance() { static Job instance; return instance; }
    void ClearPreview() { ++preview_clears; }
    void Notify(const char*) {}
    void SetState(const char* text) { state_ = text; }
    void ReleaseBuffer() {}
    void ReleaseFetchClient() {}
    bool PerformAbortDrainHome() {
        ++home_calls;
        abort_home_done_.store(true);
        abort_drain_phase_.store(AbortDrainPhase::Succeeded);
        return false;
    }
    std::string RequestAbort();
    bool StartAbortDrainHomeTask();
    bool WaitForAbortReset();
    bool ResetAbortResetState();
    void RunTail(bool ok);
};
struct Snapshot {
    bool requested, worker, busy, aborted;
    int creates;
    uint32_t epoch;
};
Snapshot Observe(const char* stage) {
    auto& job = Job::GetInstance();
    Snapshot snap{job.abort_requested_.load(), job.abort_reset_worker_active_.load(),
                  job.busy_.load(), job.state_ == "aborted", create_calls,
                  job.stream_control_epoch_.load()};
    std::fprintf(stderr,
                 "%s: abort=%d worker=%d busy=%d state=%s creates=%d epoch=%u waits=%d homes=%d\n",
                 stage, snap.requested, snap.worker, snap.busy, job.state_.c_str(),
                 snap.creates, static_cast<unsigned>(snap.epoch), wait_calls, home_calls);
    return snap;
}
void RunQueuedWorker() {
    auto& job = Job::GetInstance();
    const auto before = Observe("before-queued-worker");
    assert(before.worker && before.busy && !before.aborted);
    assert(job.stream_mutex_.try_lock());
    job.stream_mutex_.unlock();
    assert(pending != nullptr);
    std::exchange(pending, nullptr)(nullptr);
    const auto after = Observe("after-queued-worker");
    assert(!after.worker && after.busy && !after.aborted);
}
''' + product + main, stem)

    def test_repeated_abort_reuses_the_actual_worker_until_job_cleanup(self):
        self._compile_abort_harness(r'''
int main() {
    auto& job = Job::GetInstance();
    job.ota_reserved_.store(true);
    assert(job.RequestAbort().find("error") != std::string::npos);
    assert(create_calls == 0 && !job.abort_requested_.load());
    job.ota_reserved_.store(false);
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
    RunQueuedWorker();
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
    job.RunTail(false);
    assert(wait_calls == 1 && home_calls == 2 && delete_calls == 2);
    assert(pending == nullptr && !job.abort_reset_worker_active_.load());
    assert(job.state_ == "aborted" && !job.busy_.load());
    return 0;
}
''', "abort_worker_idempotence")

    def test_abort_publication_reserves_worker_before_run_cleanup(self):
        self._compile_abort_harness(r'''
int main() {
    auto& job = Job::GetInstance();
    Snapshot first{};
    bool run_completed = false;
    job.stream_mutex_.on_unlock = [&] {
        first = Observe("first-post-publication-unlock");
        assert(!job.abort_reset_owner_.Started());
        assert(home_calls == 0 && wait_calls == 0 && delete_calls == 0);
        job.last_error_ = "aborted";
        job.RunTail(false);
        run_completed = true;
        Observe("after-run-tail");
    };
    const auto reply = job.RequestAbort();
    Observe("after-request");
    assert(run_completed);
    assert(first.requested && first.worker && first.creates == 1 &&
           "abort publication unlocked before reserving its cleanup worker");
    assert(first.busy && !first.aborted && first.epoch == 1);
    assert(reply == "ok");
    assert(wait_calls == 1 && home_calls == 1 && delete_calls == 1);
    assert(pending == nullptr && !job.abort_reset_worker_active_.load());
    assert(job.abort_home_done_.load());
    assert(job.state_ == "aborted" && !job.busy_.load());
    return 0;
}
''', "abort_publication_cleanup")

    def test_abort_creation_failure_rolls_back_before_run_can_observe_cancel(self):
        self._compile_abort_harness(r'''
int main() {
    auto& job = Job::GetInstance();
    creation_succeeds = false;
    Snapshot first{};
    bool saw_unlock = false, run_completed = false;
    job.stream_mutex_.on_unlock = [&] {
        saw_unlock = true;
        first = Observe("first-unlock-with-create-failure");
        if (job.abort_requested_.load()) {
            job.RunTail(false);
            run_completed = true;
            Observe("after-run-tail");
        }
    };
    const auto reply = job.RequestAbort();
    Observe("after-failed-request");
    assert(saw_unlock);
    assert(!first.requested && first.creates == 1 &&
           "failed worker creation exposed abort before rollback");
    assert(!first.worker && first.busy && !first.aborted && first.epoch == 2);
    assert(!run_completed && wait_calls == 0 && home_calls == 0);
    assert(reply.find("error") != std::string::npos);
    assert(pending == nullptr && job.busy_.load() && job.state_ == "streaming");
    assert(!job.abort_requested_.load() && !job.abort_reset_worker_active_.load());
    creation_succeeds = true;
    assert(job.RequestAbort() == "ok");
    assert(create_calls == 2 && job.abort_reset_worker_active_.load());
    job.RunTail(false);
    assert(wait_calls == 1 && home_calls == 1 && delete_calls == 1);
    assert(job.state_ == "aborted" && !job.busy_.load());
    return 0;
}
''', "abort_creation_failure_cleanup")

    def test_abort_non_worker_modes_preserve_their_exclusions(self):
        self._compile_abort_harness(r'''
int main() {
    struct Mode {
        const char* state;
        bool awaiting, paper, pen;
        const char* reply;
    };
    const Mode modes[] = {
        {"previewing", false, false, false, "预览取消中"},
        {"awaiting_confirmation", true, false, false, "预览已取消"},
        {"downloading", false, false, false, "ok"},
        {"verifying", false, false, false, "ok"},
        {"paper_change", false, true, false, "换纸中无法即停，完成后即停"},
        {"pen_test", false, false, true, "试笔即将停止"},
    };
    auto& job = Job::GetInstance();
    for (const auto& mode : modes) {
        job.state_ = mode.state;
        job.busy_.store(true);
        job.abort_requested_.store(false);
        job.prefetch_cancel_.store(false);
        job.stream_control_epoch_.store(0);
        job.awaiting_confirmation_.store(mode.awaiting);
        job.paper_active_.store(mode.paper);
        job.pen_test_active_.store(mode.pen);
        Snapshot first{};
        bool saw_unlock = false;
        job.stream_mutex_.on_unlock = [&] {
            saw_unlock = true;
            first = Observe(mode.state);
        };
        assert(job.RequestAbort() == mode.reply);
        assert(saw_unlock && !first.worker && first.creates == 0);
        assert(first.requested == !mode.awaiting && first.busy == !mode.awaiting);
        assert(first.aborted == mode.awaiting);
        assert(first.epoch == (mode.awaiting ? 0u : 1u));
        assert(create_calls == 0 && wait_calls == 0 && home_calls == 0);
        assert(pending == nullptr && !job.abort_reset_worker_active_.load());
        assert(job.prefetch_cancel_.load() && !job.awaiting_confirmation_.load());
        assert(job.busy_.load() == !mode.awaiting);
        assert(job.state_ == (mode.awaiting ? "aborted" : mode.state));
    }
    assert(job.preview_clears == 1);
    return 0;
}
''', "abort_non_worker_modes")

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
