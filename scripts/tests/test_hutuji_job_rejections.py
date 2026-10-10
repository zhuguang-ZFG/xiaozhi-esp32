"""编译真实绘图入口与暂停/继续函数，拒绝必须可机器识别，幂等成功保持。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_hutuji_self_heal import function

ROOT = Path(os.environ.get("XIAOZHI_ROOT", Path(__file__).resolve().parents[2]))


class JobRejectionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("需要 host C++ 编译器")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = Path(cls.temp.name) / "job_rejections.cpp"
        cls.exe = cpp.with_suffix(".exe" if os.name == "nt" else "")
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        methods = "\n".join(function(source, "std::string Job::" + name + "(")
                            for name in ("StartDraw", "RequestPause", "RequestResume"))
        headers = Path(os.environ.get("XIAOZHI_HEADERS_ROOT", ROOT))
        core = (headers / "main/boards/lichuang-dev/hutuji_recovery_core.h").read_text(encoding="utf-8")
        duplicate = function(core, "inline constexpr bool IsDuplicatePreviewReentry(")
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include <utility>
struct PaperConfig {};
struct Pipe {
    static Pipe& GetInstance() { static Pipe p; return p; }
    bool IsNopaperMachine() { return true; }
    bool IsConnected() { return true; }
    bool IsReady() { return true; }
    int sends=0;
    bool SendRealtime(char) { ++sends; return true; }
};
namespace hutuji {
    bool NopaperRejectsMultiPage(bool nopaper, size_t n) { return nopaper && n>1; }
    constexpr const char* kNopaperMultiPageRejectMsg="multi";
}
struct Display { void SetStatus(const char*) {} };
struct Board {
    static Board& GetInstance() { static Board b; return b; }
    Display* GetDisplay() { return nullptr; }
};
std::string JsonString(const char* v) { return "\""+std::string(v)+"\""; }
bool IsValidDrawCapabilityUrl(const std::string&, const char*) { return true; }
bool ParseArticlePages(const std::string&, std::vector<std::pair<std::string,std::string>>&) { return false; }
std::string PaperMarker(const PaperConfig&) { return "default"; }
unsigned NextStreamControlEpoch(unsigned n) { return n+1; }
using BaseType_t=int;
constexpr int pdTRUE=1, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
BaseType_t xTaskCreateWithCaps(void (*)(void*), const char*, unsigned, void*, int, void*, int) { return pdTRUE; }
enum class StreamQuiescence { Idle };
@@DUPLICATE@@
struct Job {
    std::mutex stream_mutex_, state_mutex_;
    std::string state_="streaming", url_="old.gcode", preview_url_="old.png", last_error_, job_paper_marker_;
    std::vector<std::pair<std::string,std::string>> article_pages_;
    int article_index_=0;
    std::atomic<unsigned> article_total_{0}, article_page_{0}, stream_control_epoch_{0};
    std::atomic<int> tls_heap_fail_count_{0};
    std::atomic<StreamQuiescence> stream_quiescence_{StreamQuiescence::Idle};
    std::atomic<bool> busy_{true}, preview_worker_active_{false}, ota_reserved_{false},
        paper_update_active_{false}, speed_active_{false}, abort_hold_confirmed_{false},
        abort_requested_{false}, awaiting_confirmation_{false}, paused_{false}, paper_active_{false},
        repeat_mode_{false}, abort_home_done_{false}, buffer_replayable_{false}, prefetch_cancel_{false},
        pen_test_active_{false};
    bool finishing_at_home_=false;
    bool GetPaperConfig(PaperConfig&) { return true; }
    bool ResetAbortResetState() { return true; }
    void CancelPrefetch() {}
    void SetState(const char* s) { state_=s; }
    static void PreviewTaskEntry(void*) {}
    std::string StartDraw(const std::string&,const std::string&,const std::string&);
    std::string RequestPause();
    std::string RequestResume();
};
@@METHODS@@
int main(int argc, char** argv) {
    assert(argc==2);
    const int scenario=std::atoi(argv[1]);
    Job job;
    std::string result;
    if (scenario<3) {
        job.state_=scenario==0 ? "streaming" : "awaiting_confirmation";
        result=job.StartDraw(scenario==1 ? "old.gcode" : "new.gcode",
                             scenario==1 ? "old.png" : "new.png", "");
        assert(job.url_=="old.gcode" && job.busy_);
    } else {
        job.pen_test_active_=scenario==3 || scenario==4;
        job.paper_active_=scenario==5;
        job.paused_=scenario==6;
        if(scenario>=8){
            job.state_=scenario<10?"awaiting_confirmation":"previewing";
            job.awaiting_confirmation_=scenario<10;
        }
        result=(scenario==4 || scenario==7 || scenario==9 || scenario==11) ? job.RequestResume() : job.RequestPause();
        assert(Pipe::GetInstance().sends==0);
    }
    if (scenario==1) assert(result=="\"previewing\"");
    else if (scenario==6) assert(result==JsonString("已经是暂停状态"));
    else if (scenario==7) assert(result==JsonString("本来就没暂停"));
    else assert(result.find("\"error\":")!=std::string::npos);
}
'''
        cpp.write_text(program.replace("@@METHODS@@", methods).replace("@@DUPLICATE@@", duplicate),
                       encoding="utf-8")
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

    def test_drawing_rejects_new_artifact(self): self.run_case(0)
    def test_same_preview_is_idempotent(self): self.run_case(1)
    def test_new_artifact_cannot_replace_preview(self): self.run_case(2)
    def test_pen_test_rejects_pause(self): self.run_case(3)
    def test_pen_test_rejects_resume(self): self.run_case(4)
    def test_paper_change_rejects_pause(self): self.run_case(5)
    def test_already_paused_is_success(self): self.run_case(6)
    def test_already_resumed_is_success(self): self.run_case(7)
    def test_awaiting_rejects_pause(self): self.run_case(8)
    def test_awaiting_rejects_resume(self): self.run_case(9)
    def test_previewing_rejects_pause(self): self.run_case(10)
    def test_previewing_rejects_resume(self): self.run_case(11)


if __name__ == "__main__":
    unittest.main()
