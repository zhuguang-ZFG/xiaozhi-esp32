"""纸张纯核心、真实 JSON/NVS 事务与下载接线的 host 故障注入。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import test_hutuji_nopaper_core as core


ROOT = Path(__file__).resolve().parents[2]
DEPENDENCIES = core.ROOT
JOB = ROOT / "main/boards/lichuang-dev/hutuji_job.cc"


def _function(source, signature):
    begin = source.index(signature)
    start = source.index("{", begin)
    depth = 1
    # 本批函数无含花括号的多行原始字符串；跳过普通字符串/注释。
    import re
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', source[start + 1:])
    for token in tokens:
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if depth == 0:
                return source[begin:start + 1 + token.end()]
    raise AssertionError("函数边界未收敛")


class HutujiPaperTest(unittest.TestCase):
    def compile_run(self, source, *, json_library=False):
        compiler = core.find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        vendor = DEPENDENCIES / "managed_components/espressif__cjson/cJSON"
        if json_library and not (vendor / "cJSON.c").is_file():
            self.skipTest("缺少项目实际使用的 cJSON 源码")
        with tempfile.TemporaryDirectory() as name:
            temp = Path(name)
            src, exe = temp / "paper.cpp", temp / ("paper.exe" if os.name == "nt" else "paper")
            src.write_text(source, encoding="utf-8")
            command = [str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-I", str(ROOT), "-I", str(vendor), str(src)]
            if json_library:
                command.extend(["-x", "c++", str(vendor / "cJSON.c")])
            build = subprocess.run([*command, "-o", str(exe)], capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_dimensions_markers_and_fail_closed_limits(self):
        self.compile_run(r'''
#include <cassert>
#include <limits>
#include "main/boards/lichuang-dev/hutuji_paper_core.h"
int main() {
    using namespace hutuji;
    auto old = DefaultPaperConfig(false), mass = DefaultPaperConfig(true);
    assert(PaperMarker(old) == "p1:A4:2100:2970:0:100:1:2770:1900");
    assert(PaperMarker(mass) == "p1:A4:2100:2970:0:100:0:2050:2900");
    for (bool nopaper : {false, true}) {
        auto value = DefaultPaperConfig(nopaper);
        assert(ValidatePaperConfig(value));
        assert(PaperHeaderMatches("", value, nopaper));
        assert(!PaperHeaderMatches("", value, !nopaper));
        value.margin += 1;
        assert(ValidatePaperConfig(value));
        assert(!PaperHeaderMatches("", value, nopaper));
        assert(PaperHeaderMatches(PaperMarker(value), value, nopaper));
        assert(!PaperHeaderMatches(PaperMarker(old), value, nopaper));
    }
    for (const std::string name : {"A4", "A3", "A2", "custom"}) {
        for (bool landscape : {false, true}) for (bool swap : {false, true}) {
            PaperConfig v;
            v.paper = name;
            v.width = name == "A4" ? 2100 : name == "A3" ? 2970 : name == "A2" ? 4200 : 1805;
            v.height = name == "A4" ? 2970 : name == "A3" ? 4200 : name == "A2" ? 5940 : 2217;
            v.landscape = landscape; v.swap_xy = swap;
            v.max_x = swap ? v.PageHeight() : v.PageWidth();
            v.max_y = swap ? v.PageWidth() : v.PageHeight();
            assert(ValidatePaperConfig(v));
            PaperConfig parsed;
            assert(ParsePaperMarker(PaperMarker(v), parsed) && parsed == v);
            --v.max_x;
            assert(!ValidatePaperConfig(v));
            assert(!ParsePaperMarker(PaperMarker(v), parsed));
        }
    }
    PaperConfig parsed;
    for (const char* bad : {"", "p2:A4:2100:2970:0:100:0:2050:2900",
            "p1:A4:02100:2970:0:100:0:2050:2900", "p1:A4:2100:2970:2:100:0:2050:2900",
            "p1:A4:2100:2970:0:100:0:2050:2900:x", "p1:A4:2100:2970:0:100:0:2050:2900\n",
            "p1:A3:2100:2970:0:100:0:2050:2900", "p1:A4:2100:2970:0:-10:0:2050:2900"}) {
        assert(!ParsePaperMarker(bad, parsed));
    }
    int tenths = -1;
    assert(PaperMillimetersToTenths(0.1, tenths) && tenths == 1);
    for (double bad : {-1., 0.01, 2000.1, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()})
        assert(!PaperMillimetersToTenths(bad, tenths));
    for (const char* state : {"idle", "done", "error", "aborted"}) {
        assert(PaperChangeAllowed(state, false, false, false, false));
        assert(!PaperChangeAllowed(state, true, false, false, false));
        assert(!PaperChangeAllowed(state, false, true, false, false));
        assert(!PaperChangeAllowed(state, false, false, true, false));
        assert(!PaperChangeAllowed(state, false, false, false, true));
    }
    for (const char* state : {"previewing", "awaiting_confirmation", "streaming", "paused", "manual", ""})
        assert(!PaperChangeAllowed(state, false, false, false, false));
}
''')

    def test_real_json_parser_rejects_ambiguous_or_oversized_input(self):
        source = JOB.read_text(encoding="utf-8")
        parser = _function(source, "static bool ParsePaperConfigJson(")
        self.compile_run(r'''
#include <cassert>
#include <cstring>
#include <memory>
#include "cJSON.h"
#include "main/boards/lichuang-dev/hutuji_paper_core.h"
using namespace hutuji;
''' + parser + r'''
int main() {
    const std::string good = R"({"paper":"A4","width_mm":210,"height_mm":297,"landscape":false,"margin_mm":10,"swap_xy":false,"max_x_mm":205,"max_y_mm":290})";
    PaperConfig parsed;
    assert(ParsePaperConfigJson(good, parsed) && parsed == DefaultPaperConfig(true));
    for (const std::string suffix : {",\"paper\":\"A4\"}", ",\"extra\":1}"})
        assert(!ParsePaperConfigJson(good.substr(0, good.size() - 1) + suffix, parsed));
    for (const std::string pair : {"\"landscape\":1", "\"landscape\":\"false\"",
                                   "\"landscape\":null", "\"landscape\":[]"}) {
        auto bad = good;
        bad.replace(bad.find("\"landscape\":false"), 17, pair);
        assert(!ParsePaperConfigJson(bad, parsed));
    }
    auto hidden = good;
    hidden.replace(hidden.find("\"A4\""), 4, "\"A4\\u0000hidden\"");
    assert(!ParsePaperConfigJson(hidden, parsed));
    assert(!ParsePaperConfigJson(good + " false", parsed));
    assert(!ParsePaperConfigJson(good + std::string(513, ' '), parsed));
    assert(!ParsePaperConfigJson("{}", parsed));
    assert(!ParsePaperConfigJson("null", parsed));
}
''', json_library=True)

    def test_real_nvs_transaction_failure_recovery_and_preview_worker_guard(self):
        source = JOB.read_text(encoding="utf-8")
        functions = "\n".join(_function(source, name) for name in (
            "static bool ParsePaperConfigJson(", "Job::Job()", "bool Job::GetPaperConfig(",
            "bool Job::GetPaperJogEnvelope(", "std::string Job::RequestPaper("))
        self.compile_run(r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "cJSON.h"
#include "main/boards/lichuang-dev/hutuji_paper_core.h"
using esp_err_t = int;
using nvs_handle_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, NVS_READONLY = 0, NVS_READWRITE = 1;
constexpr unsigned kJogFreshStateTimeoutMs = 6000;
std::string stored, pending;
int fail_open = 0, fail_set = 0, fail_commit = 0, bad_read = 0, writes = 0, live_handles = 0;
bool throw_commit = false;
int nvs_open(const char*, int mode, int* handle) {
    *handle = 1;
    int result = fail_open ? 2 : mode == NVS_READONLY && stored.empty() ? ESP_ERR_NVS_NOT_FOUND : 0;
    if (result == ESP_OK) ++live_handles;
    return result;
}
int nvs_get_str(int, const char*, char* out, size_t* len) {
    if (stored.empty()) return ESP_ERR_NVS_NOT_FOUND;
    auto actual = bad_read ? "bad" : stored;
    if (actual.size() + 1 > *len) return 2;
    std::strcpy(out, actual.c_str()); *len = actual.size() + 1; return 0;
}
void nvs_close(int) { --live_handles; }
int nvs_set_str(int, const char*, const char* text) { ++writes; pending = text; return fail_set; }
int nvs_commit(int) {
    if (fail_commit) return fail_commit;
    stored = pending;
    if (throw_commit) throw std::bad_alloc();
    return 0;
}
namespace hutuji {
enum class GrblState { Idle, Run };
enum class PaperChangingState { Off, On };
class Pipe {
public:
    bool connected = true, ready = true, authorized = true, settings = true, nopaper = true, session = false;
    uint32_t connection = 3, banner = 4;
    GrblState state = GrblState::Idle;
    PaperChangingState changing = PaperChangingState::Off;
    static Pipe& GetInstance() { static Pipe p; return p; }
    bool IsConnected() const { return connected; }
    bool IsReady() const { return ready; }
    bool IsAuthorized() const { return authorized; }
    bool IsSettingsVerified() const { return settings; }
    bool IsNopaperMachine() const { return nopaper; }
    uint32_t GetConnectionSequence() const { return connection; }
    uint32_t GetResetBannerSequence() const { return banner; }
    GrblState GetGrblState() const { return state; }
    PaperChangingState GetPaperChangingState() const { return changing; }
    void SetTaskSessionActive(bool value) { session = value; }
};
struct Owner { bool running = false; bool Running() const { return running; } };
class Job {
public:
    Job();
    bool GetPaperConfig(PaperConfig&) const;
    bool GetPaperJogEnvelope(float&, float&) const;
    std::string RequestPaper(const std::string&, const std::string&, bool, bool = false);
    mutable std::mutex paper_mutex_;
    std::mutex stream_mutex_, state_mutex_;
    PaperConfig saved_paper_;
    bool paper_store_fault_ = false, paper_persisted_ = false, paper_saved_nopaper_ = false;
    mutable bool paper_machine_known_ = false, paper_last_nopaper_ = false;
    std::atomic<bool> busy_{false}, preview_worker_active_{false}, abort_reset_worker_active_{false},
        paper_active_{false}, speed_active_{false}, paper_update_active_{false},
        buffer_replayable_{true}, repeat_mode_{true};
    Owner abort_reset_owner_;
    std::string state_ = "idle", url_ = "old", preview_url_ = "old", job_paper_marker_ = "old";
    std::vector<int> article_pages_{1,2};
    int article_index_ = 2, releases = 0, cancellations = 0;
    std::atomic<int> article_page_{2}, article_total_{2};
    bool fresh = true;
    bool performance = false;
    std::function<void()> during_query;
    void StartPerformanceHold() { performance = true; }
    void StopPerformanceHold() { performance = false; }
    bool QueryAndWaitFreshMachineState(unsigned) { if (during_query) during_query(); return fresh; }
    void ReleaseBuffer() { ++releases; }
    void CancelPrefetch() { ++cancellations; }
    std::string StatusJson() const { PaperConfig c; return GetPaperConfig(c) ? PaperMarker(c) : "invalid"; }
};
''' + functions + r'''
}
int main() {
    using namespace hutuji;
    const std::string changed = R"({"paper":"A3","width_mm":297,"height_mm":420,"landscape":false,"margin_mm":10,"swap_xy":false,"max_x_mm":277,"max_y_mm":400})";
    const std::string original = PaperMarker(DefaultPaperConfig(true));
    auto& p = Pipe::GetInstance();
    Job j;
    assert(j.StatusJson() == original);
    for (int flag = 0; flag < 4; ++flag) {
        j.busy_ = flag == 0; j.preview_worker_active_ = flag == 1;
        j.abort_reset_worker_active_ = flag == 2; j.paper_active_ = flag == 3;
        assert(j.RequestPaper(changed, original, true).find("error") != std::string::npos);
        assert(writes == 0);
    }
    j.busy_ = false; j.preview_worker_active_ = false; j.abort_reset_worker_active_ = false; j.paper_active_ = false;
    assert(j.RequestPaper(changed, original, false).find("error") != std::string::npos);
    assert(j.RequestPaper(changed, "stale", true).find("error") != std::string::npos);
    assert(writes == 0);
    j.during_query = [&] { ++p.connection; };
    assert(j.RequestPaper(changed, original, true).find("error") != std::string::npos);
    assert(writes == 0 && !j.busy_ && !j.paper_update_active_ && !p.session);
    j.during_query = [] { throw std::bad_alloc(); };
    assert(j.RequestPaper(changed, original, true).find("error") != std::string::npos);
    assert(writes == 0 && !j.busy_ && !j.paper_update_active_ && !p.session && !j.performance);
    j.during_query = {};
    auto result = j.RequestPaper(changed, original, true);
    assert(result == "p1:A3:2970:4200:0:100:0:2770:4000");
    assert(writes == 1 && !j.busy_ && !j.paper_update_active_ && !p.session);
    assert(j.releases == 1 && j.cancellations == 1 && !j.buffer_replayable_ && !j.repeat_mode_);
    assert(j.url_.empty() && j.article_pages_.empty() && j.article_total_ == 0);
    Job rebooted;
    assert(rebooted.StatusJson() == result);
    p.nopaper = false;
    assert(rebooted.StatusJson() == "invalid");
    p.nopaper = true;
    for (int fault = 0; fault < 3; ++fault) {
        stored.clear(); pending.clear(); Job failed;
        fail_set = fault == 0; fail_commit = fault == 1; bad_read = fault == 2;
        assert(failed.RequestPaper(changed, original, true).find("error") != std::string::npos);
        assert(failed.StatusJson() == "invalid" && !failed.busy_ && !failed.paper_update_active_);
        fail_set = fail_commit = bad_read = 0;
        assert(failed.RequestPaper("", "invalid", false, true).find("error") != std::string::npos);
        assert(failed.RequestPaper("", "invalid", true, true) == original);
    }
    stored.clear(); pending.clear();
    Job interrupted;
    throw_commit = true;
    assert(interrupted.RequestPaper(changed, original, true).find("error") != std::string::npos);
    assert(interrupted.StatusJson() == "invalid");
    assert(!interrupted.busy_ && !interrupted.paper_update_active_ && !interrupted.performance && !p.session);
    assert(live_handles == 0);
    throw_commit = false;
    assert(interrupted.RequestPaper("", "invalid", true, true) == original);
    stored = "broken-record";
    Job corrupt;
    float x = 0, y = 0;
    assert(corrupt.StatusJson() == "invalid" && !corrupt.GetPaperJogEnvelope(x, y));
    stored.clear(); p.ready = false;
    Job booting;
    assert(booting.StatusJson() == PaperMarker(DefaultPaperConfig(false)));
    p.ready = true;
    assert(booting.StatusJson() == original);
    assert(booting.GetPaperJogEnvelope(x, y) && x == 205 && y == 290);
}
''', json_library=True)

    def test_download_repeat_and_telemetry_integration(self):
        source = JOB.read_text(encoding="utf-8")
        for signature in ("void Job::PrefetchGcode()", "bool Job::DownloadToPsram("):
            self.assertIn("CheckPaperHeader(http, marker)", _function(source, signature))
        self.assertIn("buffer_paper_marker_", _function(source, "std::string Job::RequestRepeat()"))
        self.assertIn("buffer_paper_marker_ != job_paper_marker_", _function(source, "void Job::Run()"))
        status = _function(source, "std::string Job::StatusJson()")
        self.assertIn('"paper", PaperPresentStateName', status)
        self.assertIn('"paper_config"', status)
        self.assertEqual(status.count('cJSON_AddStringToObject(root, "board",'), 1)
        self.assertIn('"device_id", SystemInfo::GetMacAddress()', status)

    def test_preview_publishes_prefetch_before_fast_confirm_and_keeps_cancel(self):
        source = JOB.read_text(encoding="utf-8")
        functions = "\n".join(_function(source, name) for name in (
            "void Job::Preview()", "std::string Job::RequestConfirm()", "std::string Job::RequestConfirmForPreview(",
            "bool Job::AdoptPrefetch()", "void Job::CancelPrefetch()"))
        self.compile_run(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr int pdTRUE = 1, MALLOC_CAP_INTERNAL = 1, kPaperStatusTimeoutMs = 10;
using BaseType_t = int;
int task_result = pdTRUE, created = 0;
std::function<void()> on_delay;
int pdMS_TO_TICKS(int value) { return value; }
void vTaskDelay(int) { if (on_delay) on_delay(); }
int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*) { ++created; return task_result; }
void heap_caps_free(void* data) { std::free(data); }
std::string JsonString(const char* value) { return std::string("\"") + value + "\""; }
struct Display { virtual ~Display() = default; };
struct LvglDisplay : Display { void ShowDrawPreviewLoading() {} };
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return nullptr; }
};
namespace hutuji {
enum class PaperChangingState { Off, On };
struct Pipe {
    static Pipe& GetInstance() { static Pipe pipe; return pipe; }
    bool IsConnected() { return true; }
    bool IsReady() { return true; }
    bool IsAuthorized() { return true; }
    bool IsSettingsVerified() { return true; }
    PaperChangingState GetPaperChangingState() { return PaperChangingState::Off; }
    bool SendLine(const char*) { return true; }
    bool WaitResponse(int, void*, int*) { return true; }
};
class Job {
public:
    enum class PrefetchState { Idle, Running, Ready };
    std::mutex stream_mutex_, prefetch_mutex_;
    std::atomic<bool> busy_{true}, awaiting_confirmation_{false}, abort_requested_{false},
                      prefetch_cancel_{false};
    std::atomic<PrefetchState> prefetch_state_{PrefetchState::Idle};
    std::atomic<uint32_t> prefetch_epoch_{1};
    std::string state_ = "previewing", url_ = "url", preview_url_ = "png",
        job_paper_marker_ = "marker", prefetch_url_, prefetch_paper_marker_, buffer_paper_marker_;
    uint8_t* prefetch_buffer_ = nullptr;
    uint8_t* buffer_ = nullptr;
    size_t prefetch_len_ = 0, buffer_len_ = 0;
    uint32_t prefetch_crc_ = 0, expect_crc_ = 0;
    std::function<void()> on_notify;
    int prefetches = 0, clears = 0;
    bool cancelled_at_fetch = false;
    void Preview();
    std::string RequestConfirm();
    std::string RequestConfirmForPreview(const std::string&, const std::string&);
    bool AdoptPrefetch();
    void CancelPrefetch();
    static void TaskEntry(void*) {}
    void WaitForAudioOutputIdle() {}
    bool DownloadAndShowPreview(const std::string&) { return true; }
    void ClearPreview() { ++clears; }
    void SetState(const std::string& state) { state_ = state; }
    void Notify(const std::string&) { if (on_notify) on_notify(); }
    void ReleaseFetchClient() {}
    void PrefetchGcode() {
        ++prefetches;
        cancelled_at_fetch = prefetch_cancel_.load();
        prefetch_state_ = PrefetchState::Idle;
    }
};
''' + functions + r'''
}
int main() {
    using namespace hutuji;
    Job mismatched;
    mismatched.awaiting_confirmation_ = true;
    assert(mismatched.RequestConfirmForPreview("other", "png").find("error") != std::string::npos);
    assert(mismatched.RequestConfirmForPreview("url", "").find("error") != std::string::npos);
    assert(created == 0 && mismatched.awaiting_confirmation_ && mismatched.busy_);
    assert(mismatched.RequestConfirmForPreview("url", "png") == "\"started\"");
    assert(created == 1 && !mismatched.awaiting_confirmation_);
    created = 0;
    Job fast;
    fast.on_notify = [&] {
        // 通知发出后，真实界面可立刻触发确认；预取必须已属于同一次事务。
        assert(fast.awaiting_confirmation_);
        assert(fast.prefetch_state_ == Job::PrefetchState::Running);
        assert(fast.RequestConfirm() == "\"started\"");
        int waits = 0;
        on_delay = [&] { ++waits; fast.PrefetchGcode(); };
        assert(!fast.AdoptPrefetch());
        on_delay = {};
        assert(waits == 1);
    };
    fast.Preview();
    assert(created == 1 && !fast.awaiting_confirmation_);
    for (auto phase : {Job::PrefetchState::Ready, Job::PrefetchState::Idle}) {
        Job failed;
        failed.awaiting_confirmation_ = true;
        failed.prefetch_state_ = phase;
        task_result = 0;
        assert(failed.RequestConfirm().find("error") != std::string::npos);
        assert(failed.awaiting_confirmation_ && failed.busy_);
        assert(failed.prefetch_state_ == phase && failed.clears == 0);
    }
    task_result = pdTRUE;
    Job cancelled;
    cancelled.on_notify = [&] {
        // 取消待确认时不需要机械停止，但旧预取不得重新清掉取消标记。
        cancelled.awaiting_confirmation_ = false;
        cancelled.busy_ = false;
        cancelled.CancelPrefetch();
    };
    cancelled.Preview();
    assert(cancelled.prefetches == 1 && cancelled.cancelled_at_fetch);
    assert(cancelled.prefetch_state_ == Job::PrefetchState::Idle && !cancelled.busy_);
}
''')


if __name__ == "__main__":
    unittest.main()
