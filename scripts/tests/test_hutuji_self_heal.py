"""编译真实自愈函数，注入宽限期/通知期接管并核对 WithCaps 资源回收。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get("XIAOZHI_ROOT", Path(__file__).resolve().parents[2]))


def function(source, signature):
    start = source.index(signature)
    left = source.index("{", start)
    depth = 1
    right = left + 1
    while depth:
        depth += (source[right] == "{") - (source[right] == "}")
        right += 1
    return source[start:right]


class SelfHealTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("需要 host C++ 编译器")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = Path(cls.temp.name) / "self_heal.cpp"
        cls.exe = cpp.with_suffix(".exe" if os.name == "nt" else "")
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        methods = "\n".join(function(source, signature) for signature in (
            "void Job::StartSelfHealTask(", "void Job::SelfHealTaskEntry(", "void Job::RunSelfHeal("))
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <mutex>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
using BaseType_t = int;
constexpr int pdTRUE=1, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
constexpr unsigned kSelfHealGraceMs=10000, kSelfHealNotifyMs=20000;
constexpr int kTlsHeapFailSelfHealThreshold=2;
struct Mutex {
    bool locked=false, contested=false;
    void lock() { assert(!locked && !contested); locked=true; }
    bool try_lock() { if (locked || contested) return false; locked=true; return true; }
    void unlock() { assert(locked); locked=false; }
};
// 将 std::mutex 替换成可观察同一锁域的测试互斥量，业务函数保持原文。
struct Job {
    Mutex stream_mutex_, state_mutex_;
    std::string state_="error";
    std::atomic<bool> busy_{false}, ota_reserved_{false}, self_heal_pending_{true},
        preview_worker_active_{false}, abort_reset_worker_active_{false}, paper_active_{false};
    std::atomic<int> tls_heap_fail_count_{2};
    struct Owner { bool running=false; bool Running() { return running; } } abort_reset_owner_;
    int notices=0;
    void Notify(const char*) { ++notices; }
    void StartSelfHealTask();
    static void SelfHealTaskEntry(void*);
    void RunSelfHeal();
};
static Job* current;
static int scenario, waits, restarts, live_allocations;
static bool restart_owned=false;
static void (*entry)(void*);
static void* entry_arg;
BaseType_t xTaskCreateWithCaps(void (*fn)(void*), const char*, unsigned, void* arg,
                              int, void*, int caps) {
    assert(caps & MALLOC_CAP_SPIRAM);
    if (scenario==7) return 0;
    ++live_allocations;
    entry=fn; entry_arg=arg;
    return pdTRUE;
}
void vTaskDelete(void*) {}
void vTaskDeleteWithCaps(void*) { --live_allocations; }
void vTaskDelay(unsigned ms) {
    ++waits;
    assert(ms==(waits==1 ? kSelfHealGraceMs : kSelfHealNotifyMs));
    if (scenario==1 && waits==1) current->state_="idle";
    if (scenario==2 && waits==2) current->busy_=true;
    if (scenario==3 && waits==2) current->ota_reserved_=true;
    if (scenario==4 && waits==2) current->tls_heap_fail_count_=0;
    if (scenario==5 && waits==2) current->preview_worker_active_=true;
    if (scenario==6 && waits==2) current->stream_mutex_.contested=true;
    if (scenario==8 && waits==2) current->abort_reset_owner_.running=true;
}
void esp_restart() { ++restarts; restart_owned=current->stream_mutex_.locked; }
@@METHODS@@
int main(int argc, char** argv) {
    assert(argc==2);
    scenario=std::atoi(argv[1]);
    Job job; current=&job;
    job.StartSelfHealTask();
    if (entry) entry(entry_arg);
    assert(restarts==(scenario==0 ? 1 : 0));
    if (scenario==0) assert(restart_owned); // 从最终复核到重启始终排斥运动/OTA 发布。
    else assert(!job.self_heal_pending_);
    assert(live_allocations==0); // 放弃路径必须回收 WithCaps 栈与 TCB。
    assert(job.notices==((scenario==1 || scenario==7) ? 0 : 1));
}
'''
        # std::lock_guard/unique_lock 的模板参数也沿用同一个替身互斥量。
        program = program.replace("@@METHODS@@", methods.replace("std::mutex", "Mutex"))
        cpp.write_text(program, encoding="utf-8")
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.env.get("PATH", "")
        built = subprocess.run([compiler, "-std=c++17", str(cpp), "-o", str(cls.exe)],
                               capture_output=True, text=True, env=cls.env, timeout=60)
        if built.returncode:
            raise AssertionError(built.stderr)

    def run_case(self, scenario):
        result = subprocess.run([str(self.exe), str(scenario)], capture_output=True,
                                text=True, env=self.env, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_restart_owns_motion_reservation(self): self.run_case(0)
    def test_grace_cancel_frees_psram_task(self): self.run_case(1)
    def test_motion_takeover_cancels_restart(self): self.run_case(2)
    def test_ota_takeover_cancels_restart(self): self.run_case(3)
    def test_recovered_heap_cancels_old_restart(self): self.run_case(4)
    def test_preview_cleanup_cancels_restart(self): self.run_case(5)
    def test_contended_motion_lock_cancels_restart(self): self.run_case(6)
    def test_failed_task_creation_releases_pending(self): self.run_case(7)
    def test_abort_owner_cancels_restart(self): self.run_case(8)


if __name__ == "__main__":
    unittest.main()
