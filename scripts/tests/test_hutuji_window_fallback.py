"""编译真实位置确认及超时收尾分支：Z到位、迟到错误与受控停止。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_hutuji_abort_home_ack import function

ROOT = Path(os.environ.get("XIAOZHI_ROOT", Path(__file__).resolve().parents[2]))


class WindowFallbackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX") or shutil.which("g++")
        if not compiler: raise RuntimeError("需要host C++编译器")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        stem = Path(cls.temp.name) / "window_fallback"
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        stream = function(source, "bool Job::StreamToGrbl(")
        marker = stream.index("// Timeout")
        begin = stream.rfind("{", 0, marker)
        tail = function(stream[begin:], "{")[1:-1]
        fail = function(stream, "auto fail_window_and_stop") + ";"
        guard = function(stream, "struct WindowGuard") + ";"
        success = stream[stream.rfind("\n    }") + len("\n    }"):-1]
        helpers = function(source, "static bool ExtractGcodeWord(") + "\n"
        helpers += function(source, "bool Job::ConfirmInFlightDoneByStatus(")
        core = (ROOT / "main/boards/lichuang-dev/hutuji_recovery_core.h").read_text(encoding="utf-8")
        finish = function(core, "inline constexpr StreamQuiescence FinishStream(")
        program = r'''
#include <atomic>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
constexpr uint32_t kOkFallbackIdleTimeoutMs=2000;
constexpr float kOkFallbackPosTolMm=0.05f;
constexpr int kMaxOkFallback=3;
enum class GrblState { Idle, Run, Alarm };
enum class WaitResult { Ok, Failed, Deferred, Timeout };
enum class StreamQuiescence { Idle, Active, Quiesced, Failed };
@@FINISH@@
static int scenario;
struct Pipe {
    bool ready=true, connected=true;
    uint32_t connection=3;
    float x=10,y=20,z=0;
    GrblState state=GrblState::Idle;
    int holds=0;
    std::deque<WaitResult> replies;
    static Pipe& GetInstance() { static Pipe p; return p; }
    bool IsConnected() { return connected; }
    bool IsReady() { return ready; }
    uint32_t GetConnectionSequence() { return connection; }
    GrblState GetGrblState() { return state; }
    void GetMachinePos(float& xx,float& yy,float& zz) { xx=x; yy=y; zz=z; }
    bool SendRealtime(char c) { if(c=='!') ++holds; return true; }
    void SetWindowed(bool) {}
    void DrainResponses() { replies.clear(); }
    WaitResult TakeResponse(uint32_t timeout,int* error) {
        assert(timeout==0);
        auto reply=replies.empty() ? WaitResult::Timeout : replies.front();
        if (!replies.empty()) replies.pop_front();
        *error=reply==WaitResult::Deferred ? 8 : reply==WaitResult::Failed ? 9 : -1;
        return reply;
    }
};
using LineSpan=std::string;
struct Job {
    uint32_t stream_connection_seq_=3;
    bool stream_disconnected_=false;
    std::atomic<bool> stream_error_stop_required_{false};
    std::string last_error_;
    size_t lines_sent_=0;
    bool quiesced=false;
    std::string_view LineAt(const LineSpan& line) { return line; }
    void UpdateDisplayProgress() {}
    bool QueryAndWaitFreshMachineState(uint32_t timeout) {
        assert(timeout==kOkFallbackIdleTimeoutMs);
        auto& p=Pipe::GetInstance();
        if(scenario==20) p.replies={WaitResult::Ok,WaitResult::Failed};
        if(scenario==21) p.replies={WaitResult::Deferred};
        if(scenario==22) p.replies={WaitResult::Ok};
        if(scenario==24) p.replies={WaitResult::Failed};
        if(scenario==26) p.replies={WaitResult::Ok,WaitResult::Ok,WaitResult::Ok};
        if(scenario==27) ++p.connection;
        return scenario!=8 && scenario!=24 && scenario!=25 && scenario!=27;
    }
    bool ConfirmInFlightDoneByStatus(const std::vector<LineSpan>&,size_t,size_t);
    bool RunTimeoutBranch();
    void VerifySuccessTail(bool normal);
};
@@HELPERS@@
void Job::VerifySuccessTail(bool normal) {
    std::mutex stream_mutex_;
    std::atomic<StreamQuiescence> stream_quiescence_{StreamQuiescence::Active};
    auto& pipe=Pipe::GetInstance();
    bool result=[&]() {
        @@GUARD@@
        WindowGuard window_guard{pipe,stream_mutex_,stream_quiescence_};
        if(!normal) return false;
        @@SUCCESS@@
    }();
    assert(result==normal);
    assert(stream_quiescence_==(normal ? StreamQuiescence::Quiesced : StreamQuiescence::Failed));
}
bool Job::RunTimeoutBranch() {
    auto& pipe=Pipe::GetInstance();
    std::vector<LineSpan> spans={"G1 X10 Y20 F24000","G1G90 Z0.0F10000"};
    std::deque<size_t> c_line{8,8};
    std::deque<uint32_t> c_line_tick{0,0};
    size_t c_line_bytes_sum=16;
    int ok_fallback_count=scenario==28 ? kMaxOkFallback : 0;
    struct Guard { bool& value; void MarkQuiesced() { value=true; } } window_guard{quiesced};
    @@FAIL@@
    // 只运行真实流循环的一轮超时分支；continue代表释放旧窗口后可进入下一轮。
    for(int pass=0;pass<1;++pass) {
        @@TAIL@@
    }
    return true;
}
int main(int argc,char** argv) {
    assert(argc==2); scenario=std::atoi(argv[1]);
    Job job; auto& pipe=Pipe::GetInstance();
    if(scenario>=40) { job.VerifySuccessTail(scenario==40); return 0; }
    if(scenario<20) {
        std::vector<LineSpan> spans={"G1G90 Z5.0F10000"};
        if(scenario==2) pipe.z=5;
        if(scenario==3) spans.push_back("G1 X10 Y20 F24000");
        if(scenario==4) { spans={"G1 X99 Y20 F24000"}; }
        if(scenario==5) spans={"G21","G90"};
        if(scenario==6) spans.push_back("G1G90 Z0.0F10000");
        if(scenario==7) pipe.z=4.7f;
        if(scenario==9) { pipe.z=5; pipe.state=GrblState::Run; }
        bool result=job.ConfirmInFlightDoneByStatus(spans,0,spans.size());
        assert(result==(scenario==2 || scenario==5 || scenario==6));
    } else {
        if(scenario==23) pipe.z=5;
        if(scenario==28) pipe.replies={WaitResult::Failed};
        bool result=job.RunTimeoutBranch();
        assert(result==(scenario==22));
        if(scenario==22) { assert(job.lines_sent_==2 && pipe.holds==0); }
        else if(scenario==27) { assert(job.stream_disconnected_ && pipe.holds==0); }
        else {
            assert(pipe.holds==1 && job.stream_error_stop_required_ && job.quiesced);
            assert(job.lines_sent_==0);
            if(scenario==20 || scenario==24 || scenario==28) assert(job.last_error_.find("9")!=std::string::npos);
            if(scenario==21) assert(job.last_error_.find("8")!=std::string::npos);
        }
    }
}
'''
        program = program.replace("@@HELPERS@@",helpers).replace("@@FAIL@@",fail).replace("@@TAIL@@",tail)
        program = program.replace("@@GUARD@@",guard).replace("@@SUCCESS@@",success)
        program = program.replace("@@FINISH@@",finish)
        stem.with_suffix(".cpp").write_text(program,encoding="utf-8")
        cls.exe=stem.with_suffix(".exe" if os.name=="nt" else "")
        cls.env=dict(os.environ)
        cls.env["PATH"]=str(Path(compiler).parent)+os.pathsep+cls.env.get("PATH", "")
        built=subprocess.run([compiler,"-std=c++17",str(stem.with_suffix(".cpp")),"-o",str(cls.exe)],
                             capture_output=True,text=True,env=cls.env,timeout=60)
        if built.returncode: raise AssertionError(built.stderr)

    def run_case(self, scenario):
        result=subprocess.run([str(self.exe),str(scenario)],capture_output=True,text=True,env=self.env,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_pure_z_must_reach_target(self): self.run_case(1)
    def test_pure_z_matching_can_settle(self): self.run_case(2)
    def test_last_xy_does_not_hide_prior_z_target(self): self.run_case(3)
    def test_xy_mismatch_still_rejected(self): self.run_case(4)
    def test_modal_only_compatibility(self): self.run_case(5)
    def test_last_z_wins(self): self.run_case(6)
    def test_z_position_tolerance(self): self.run_case(7)
    def test_missing_fresh_position(self): self.run_case(8)
    def test_running_is_not_complete(self): self.run_case(9)
    def test_late_error_cannot_be_discarded(self): self.run_case(20)
    def test_late_deferred_is_error_in_window(self): self.run_case(21)
    def test_late_ok_keeps_valid_fallback(self): self.run_case(22)
    def test_unknown_z_completion_stops_physically(self): self.run_case(23)
    def test_error_after_failed_query_still_wins(self): self.run_case(24)
    def test_timeout_stops_before_releasing_busy(self): self.run_case(25)
    def test_surplus_replies_fail_closed(self): self.run_case(26)
    def test_new_connection_is_not_stopped_as_old_window(self): self.run_case(27)
    def test_error_not_hidden_by_fallback_limit(self): self.run_case(28)
    def test_normal_stream_exit_publishes_quiesced(self): self.run_case(40)
    def test_abnormal_exit_still_blocks_reset(self): self.run_case(41)


if __name__ == "__main__": unittest.main()
