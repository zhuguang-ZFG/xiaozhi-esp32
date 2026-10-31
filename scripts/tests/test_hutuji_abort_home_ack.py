"""编译真实归位与逐行发送器：应答立即/迟到、拒绝与换连接均不能串行。"""
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
    depth, right = 1, left + 1
    while depth:
        depth += (source[right] == "{") - (source[right] == "}")
        right += 1
    return source[start:right]


class AbortHomeAckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("需要host C++编译器")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        stem = Path(cls.temp.name) / "abort_home"
        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        pipe = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(encoding="utf-8")
        methods = "\n".join(function(pipe, signature) for signature in (
            "bool Pipe::SendLine(", "bool Pipe::IsCommandSessionCurrent(",
            "bool Pipe::SendLineForSession(", "bool Pipe::SendLineLocked("))
        methods += "\n" + function(job, "bool Job::HomeAfterAbort(")
        program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define HUTUJI_QUIET_STREAM_LOG 1
using TickType_t=uint32_t;
constexpr uint32_t portTICK_PERIOD_MS=1, kHomeOkTimeoutMs=100, kHomeIdleTimeoutMs=100;
TickType_t xTaskGetTickCount() { return 0; }
bool IsStreamingMotionLine(const std::string&) { return true; }
enum class WaitResult { Ok, Failed, Timeout };
static int scenario;
struct Pipe {
    std::atomic<bool> connected_{true}, ready_{true}, authorized_{true}, settings_verified_{true},
        task_session_active_{true}, drain_on_send_{true};
    std::atomic<uint32_t> connection_seq_{3}, reset_banner_seq_{5};
    std::mutex write_mutex_;
    std::vector<std::string> sent;
    std::deque<WaitResult> replies, delayed;
    int waits=0, shutdowns=0;
    static Pipe& GetInstance() { static Pipe p; return p; }
    bool IsNopaperMachine() { return true; }
    bool IsConnected() { return connected_; }
    bool IsReady() { return ready_; }
    bool IsAuthorized() { return authorized_; }
    bool IsSettingsVerified() { return settings_verified_; }
    uint32_t GetConnectionSequence() { return connection_seq_; }
    uint32_t GetResetBannerSequence() { return reset_banner_seq_; }
private:
    bool IsCommandSessionCurrent(uint32_t,uint32_t) const;
public:
    bool SendLine(const std::string&);
    bool SendLineForSession(const std::string&,uint32_t,uint32_t);
    bool SendLineLocked(const std::string&);
    void DrainResponses() { replies.clear(); }
    void ShutdownSocket(uint32_t connection) {
        if (connection_seq_==connection) { ++shutdowns; connected_=false; }
    }
    bool SendRawLocked(const char* data,size_t size) {
        sent.emplace_back(data,size);
        const bool first=sent.size()==1;
        if ((scenario==3 && first) || (scenario==6 && !first)) return true;
        auto value=(scenario==2 && first) ? WaitResult::Failed : WaitResult::Ok;
        (scenario==1 ? delayed : replies).push_back(value);
        return true;
    }
    WaitResult WaitResponse(uint32_t,std::string*=nullptr,int*=nullptr);
};
WaitResult Pipe::WaitResponse(uint32_t, std::string*, int* error) {
    ++waits;
    if (replies.empty() && !delayed.empty()) { replies=delayed; delayed.clear(); }
    auto result=replies.empty() ? WaitResult::Timeout : replies.front();
    if (!replies.empty()) replies.pop_front();
    if (error) *error=result==WaitResult::Failed ? 9 : -1;
    if (waits==1 && (scenario==4 || scenario==8)) ++connection_seq_;
    if (waits==1 && scenario==5) ++reset_banner_seq_;
    if (scenario==8) return WaitResult::Timeout;
    return result;
}
struct Job {
    std::mutex stream_mutex_;
    std::string last_error_;
    int releases=0;
    bool HomeAfterAbort(float,float);
    bool WaitForIdle(bool,uint32_t) { return scenario!=7; }
    bool ReleaseMotorsAfterHome(uint32_t connection,uint32_t banner,bool) {
        assert(connection==3 && banner==5); ++releases; return true;
    }
};
@@METHODS@@
int main(int argc,char** argv) {
    assert(argc==2); scenario=std::atoi(argv[1]);
    Job job; auto& pipe=Pipe::GetInstance();
    bool result=job.HomeAfterAbort(40,50);
    assert(result==(scenario==0 || scenario==1));
    assert(pipe.sent[0]=="G92 X40.000 Y50.000 Z0\n");
    if (scenario>=2 && scenario<=5 || scenario==8) assert(pipe.sent.size()==1);
    else assert(pipe.sent.size()==2 && pipe.sent[1]=="G1G90 X0Y0F8000\n");
    assert(job.releases==(result ? 1 : 0));
    if (scenario==2 || scenario==3 || scenario==5 || scenario==6 || scenario==7) assert(pipe.shutdowns==1);
    if (scenario==4 || scenario==8) assert(pipe.shutdowns==0);
}
'''.replace("std::string*=nullptr,int*=nullptr", "std::string* = nullptr,int* = nullptr")
        stem.with_suffix(".cpp").write_text(program.replace("@@METHODS@@", methods), encoding="utf-8")
        cls.exe = stem.with_suffix(".exe" if os.name=="nt" else "")
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent)+os.pathsep+cls.env.get("PATH", "")
        built = subprocess.run([compiler,"-std=c++17",str(stem.with_suffix(".cpp")),"-o",str(cls.exe)],
                               capture_output=True,text=True,env=cls.env,timeout=60)
        if built.returncode: raise AssertionError(built.stderr)

    def run_case(self, scenario):
        result = subprocess.run([str(self.exe),str(scenario)],capture_output=True,text=True,env=self.env,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_immediate_ack_is_not_cleared_by_home(self): self.run_case(0)
    def test_delayed_ack_still_homes(self): self.run_case(1)
    def test_coordinate_rejection_prevents_motion(self): self.run_case(2)
    def test_coordinate_timeout_prevents_motion(self): self.run_case(3)
    def test_reconnect_between_commands_does_not_move_new_peer(self): self.run_case(4)
    def test_reset_between_commands_prevents_motion(self): self.run_case(5)
    def test_home_timeout_closes_original_session(self): self.run_case(6)
    def test_idle_timeout_does_not_release_motors(self): self.run_case(7)
    def test_timeout_cannot_close_a_new_connection(self): self.run_case(8)


if __name__ == "__main__": unittest.main()
