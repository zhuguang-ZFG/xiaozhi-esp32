"""编译真实下载函数，注入内存/网络异常并核对资源与确认竞态。"""
import unittest

import test_hutuji_paper as paper

JOB = paper.JOB
_function = paper._function


SHIM = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr int kFetchMaxAttempts = 3, kFetchRetryBackoffMs[] = {1, 2};
constexpr int kFetchFirstTimeoutMs = 10, kFetchRetryTimeoutMs = 20;
constexpr size_t kMaxGcodeBytes = 524288, kMaxPreviewBytes = 262144;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
enum class FetchOutcome { kFatal, kRetryable };
std::set<void*> allocations;
int failure = 0, releases = 0, deletes = 0, decoder_opened = 0, decoder_closed = 0;
std::function<void()> on_read, on_release;
void* heap_caps_malloc(size_t size, int) {
    void* data = std::malloc(size); assert(data); allocations.insert(data); return data;
}
void heap_caps_free(void* data) {
    if (data) { assert(allocations.erase(data) == 1); std::free(data); }
}
int pdMS_TO_TICKS(int value) { return value; }
void vTaskDelay(int) {}
void vTaskDeleteWithCaps(void*) { ++deletes; }
bool ParseCrc32Header(const std::string&, uint32_t& value) { value = 1; return true; }
uint32_t Crc32Ieee(const uint8_t*, size_t) { return 1; }
bool Crc32Matches(uint32_t a, uint32_t b) { return a == b; }
struct Http {
    void SetTimeout(int) {}
    bool Open(const char*, const std::string&) { if (failure == 1) throw std::bad_alloc(); return true; }
    int GetStatusCode() { return 200; }
    size_t GetBodyLength() { return 16; }
    std::string GetResponseHeader(const char*) { if (failure == 2) throw std::bad_alloc(); return "1"; }
    int Read(char* data, size_t size) {
        if (on_read) on_read();
        if (failure == 3) throw std::bad_alloc();
        std::memset(data, 1, size); return static_cast<int>(size);
    }
    void Close() {}
};
struct Header { unsigned long w = 4, h = 4; int cf = 1, stride = 8; };
struct lv_draw_buf_t { uint8_t* data = nullptr; size_t data_size = 32; Header header; };
struct lv_image_decoder_dsc_t { void* decoded = nullptr; const void* source = nullptr; };
constexpr int LV_RESULT_OK = 0, LV_COLOR_FORMAT_RGB565 = 1;
int lv_image_decoder_open(lv_image_decoder_dsc_t* probe, const void* source, void*) {
    static uint8_t bytes[32]{};
    static lv_draw_buf_t decoded{bytes, 32, {}};
    probe->decoded = &decoded; probe->source = source; ++decoder_opened; return LV_RESULT_OK;
}
void lv_image_decoder_close(lv_image_decoder_dsc_t* probe) {
    assert(allocations.count(const_cast<void*>(probe->source)) == 1); ++decoder_closed;
}
struct LvglAllocatedImage {
    void* data;
    LvglAllocatedImage(void* value, size_t) : data(value) { if (failure == 4) throw std::bad_alloc(); }
    LvglAllocatedImage(void* value, size_t, int, int, int, int) : data(value) {
        if (failure == 5) throw std::bad_alloc();
    }
    ~LvglAllocatedImage() { heap_caps_free(data); }
    const void* image_dsc() { return data; }
};
struct Display { virtual ~Display() = default; void ShowNotification(const char*) {} };
struct LvglDisplay : Display {
    std::unique_ptr<LvglAllocatedImage> image;
    void ShowDrawPreview(std::unique_ptr<LvglAllocatedImage> value, const char*,
                         std::function<void()>, std::function<void()>) { image = std::move(value); }
};
struct Board {
    LvglDisplay display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
    void* GetNetwork() { return this; }
};
struct SystemInfo { static void LogHeapNow(const char*) {} };
struct Application {
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()>) {}
};
struct McpServer {
    static McpServer& GetInstance() { static McpServer server; return server; }
    bool ScheduleBackground(std::function<void()>) { return true; }
};
struct cJSON { const char* valuestring; };
cJSON* cJSON_Parse(const char*) { return nullptr; }
const cJSON* cJSON_GetObjectItemCaseSensitive(cJSON*, const char*) { return nullptr; }
bool cJSON_IsString(const cJSON*) { return false; }
void cJSON_Delete(cJSON*) {}
namespace Lang { namespace Strings { constexpr const char* DRAW_PREVIEW_HINT = "hint"; } }
namespace hutuji {
class Job {
public:
    enum class PrefetchState { Idle, Running, Ready };
    std::mutex stream_mutex_, state_mutex_, prefetch_mutex_, fetch_mutex_;
    std::atomic<bool> preview_worker_active_{true}, busy_{true}, awaiting_confirmation_{false},
                      abort_requested_{false}, prefetch_cancel_{false};
    std::atomic<PrefetchState> prefetch_state_{PrefetchState::Running};
    std::atomic<uint32_t> prefetch_epoch_{1};
    std::string state_ = "previewing", url_ = "url", job_paper_marker_ = "marker",
        prefetch_url_, prefetch_paper_marker_, buffer_paper_marker_;
    uint8_t* prefetch_buffer_ = nullptr;
    uint8_t* buffer_ = nullptr;
    size_t prefetch_len_ = 0, buffer_len_ = 0;
    uint32_t prefetch_crc_ = 0, expect_crc_ = 0;
    bool performance = true, fail_display = false;
    static Job& GetInstance() { static Job job; return job; }
    static void PreviewTaskEntry(void*);
    void PrefetchGcode();
    bool AdoptPrefetch();
    void CancelPrefetch();
    bool DownloadAndShowPreview(const std::string&);
    void Preview() { throw std::bad_alloc(); }
    void WaitForAudioOutputIdle() {}
    bool WaitForTlsHeapBudget() { return true; }
    void ReleaseFetchClient() { ++releases; if (on_release) on_release(); }
    Http* AcquireFetchClient() { static Http http; return &http; }
    bool CheckPaperHeader(Http*, std::string& marker) { marker = "marker"; return true; }
    void StopPerformanceHold() { performance = false; }
    void ClearPreview() { if (fail_display) throw std::bad_alloc(); }
    void SetState(const char* value) { state_ = value; }
    void Notify(const char*) {}
    std::string RequestConfirm() { return "{}"; }
    void RequestAbort() {}
};
'''


class HutujiPreviewFailureTest(unittest.TestCase):
    compile_run = paper.HutujiPaperTest.compile_run

    def test_prefetch_exception_releases_psram_and_unblocks_confirmation(self):
        source = JOB.read_text(encoding="utf-8")
        functions = "\n".join(_function(source, name) for name in (
            "void Job::PrefetchGcode(", "void Job::CancelPrefetch(", "bool Job::AdoptPrefetch("))
        self.compile_run(SHIM + functions + r'''
}
int main() {
    using namespace hutuji;
    for (int stage : {1, 2, 3}) {
        failure = stage;
        Job job;
        const int before = releases;
        on_release = [&] { assert(job.prefetch_state_ == Job::PrefetchState::Running); };
        job.PrefetchGcode();
        on_release = {};
        assert(job.prefetch_state_ == Job::PrefetchState::Idle);
        assert(allocations.empty() && releases > before);
    }
    failure = 0;
    Job cancelled;
    on_read = [&] { cancelled.prefetch_cancel_ = true; ++cancelled.prefetch_epoch_; };
    cancelled.PrefetchGcode();
    on_read = {};
    assert(cancelled.prefetch_buffer_ == nullptr && allocations.empty());
    for (int i = 0; i < 2000; ++i) {
        Job job;
        on_release = [&] { assert(job.prefetch_state_ == Job::PrefetchState::Running); };
        job.PrefetchGcode();
        on_release = {};
        assert(job.prefetch_state_ == Job::PrefetchState::Ready && allocations.size() == 1);
        job.buffer_ = static_cast<uint8_t*>(heap_caps_malloc(8, 0));
        assert(job.AdoptPrefetch());
        assert(job.buffer_paper_marker_ == "marker" && job.buffer_len_ == 16);
        assert(allocations.size() == 1);
        heap_caps_free(job.buffer_);
        job.buffer_ = nullptr;
        job.CancelPrefetch();
        assert(allocations.empty());
    }
}
''')

    def test_preview_task_exception_settles_owned_phase_without_stopping_confirmed_draw(self):
        source = JOB.read_text(encoding="utf-8")
        functions = "\n".join(_function(source, name) for name in (
            "void Job::PreviewTaskEntry(", "void Job::CancelPrefetch("))
        self.compile_run(SHIM + functions + r'''
}
int main() {
    using namespace hutuji;
    for (bool display_failure : {false, true}) {
        Job job;
        job.fail_display = display_failure;
        Job::PreviewTaskEntry(&job);
        assert(!job.busy_ && !job.preview_worker_active_ && !job.awaiting_confirmation_);
        assert(job.state_ == "error" && !job.performance);
    }
    Job awaiting;
    awaiting.state_ = "awaiting_confirmation"; awaiting.awaiting_confirmation_ = true;
    awaiting.abort_requested_ = true;
    Job::PreviewTaskEntry(&awaiting);
    assert(awaiting.state_ == "aborted" && !awaiting.busy_ && !awaiting.abort_requested_);
    Job confirmed;
    confirmed.state_ = "downloading";
    Job::PreviewTaskEntry(&confirmed);
    assert(confirmed.busy_ && confirmed.state_ == "downloading" && confirmed.performance);
    assert(!confirmed.preview_worker_active_ && confirmed.prefetch_state_ == Job::PrefetchState::Idle);
    assert(deletes == 4);
}
''')

    def test_preview_png_and_decoder_cleanup_on_allocation_failure(self):
        source = JOB.read_text(encoding="utf-8")
        function = _function(source, "bool Job::DownloadAndShowPreview(")
        self.compile_run(SHIM + function + r'''
}
int main() {
    using namespace hutuji;
    for (int stage : {1, 2, 3, 4, 5, 0}) {
        failure = stage;
        Job job;
        try { (void)job.DownloadAndShowPreview("preview"); } catch (const std::exception&) {}
        Board::GetInstance().display.image.reset();
        assert(allocations.empty());
        assert(decoder_opened == decoder_closed);
    }
}
''')


if __name__ == "__main__":
    unittest.main()
