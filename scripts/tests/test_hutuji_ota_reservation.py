"""编译当前真实函数，硬件接口替身仅用于确定性交错回归。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

def function(source, signature):
    start = source.index(signature)
    left = source.index("{", start)
    depth, right = 1, left + 1
    while depth:
        depth += (source[right] == "{") - (source[right] == "}")
        right += 1
    return source[start:right]

def compile_run(program):
    compiler = os.environ.get("CXX") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("需要 host C++ 编译器，不跳过关键并发回归")
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory() as directory:
        cpp = Path(directory) / "probe.cpp"
        exe = Path(directory) / ("probe.exe" if os.name == "nt" else "probe")
        cpp.write_text(program, encoding="utf-8")
        built = subprocess.run([compiler, "-std=c++17", "-pthread", "-I", str(ROOT), str(cpp), "-o", str(exe)],
                               capture_output=True, text=True, env=env, timeout=60)
        assert built.returncode == 0, built.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=10)
        assert result.returncode == 0, result.stderr

PROGRAM = r'''
#include <cassert>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdexcept>
#include "main/boards/lichuang-dev/hutuji_speed_core.h"
using hutuji::IsSpeedSettledState;
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cstring>
#define BOARD_NAME "test-board"
#define ESP_LOGE(...) ((void)0)
const int kPropertyTypeString = 1;
struct Property {
    std::string name, data;
    Property(std::string n, int, std::string d=""): name(n), data(d) {}
    template<typename T> const T& value() const { return data; }
};
struct PropertyList {
    std::map<std::string, Property> fields;
    PropertyList() = default;
    PropertyList(std::initializer_list<Property> p) { for (const auto& v:p) fields.emplace(v.name,v); }
    const Property& operator[](const char* k) const { return fields.at(k); }
};
using ReturnValue = std::string;
struct McpServer {
    std::map<std::string, std::function<ReturnValue(const PropertyList&)>> callbacks;
    void AddTool(std::string n, std::string, PropertyList, std::function<ReturnValue(const PropertyList&)> f) { callbacks[n]=f; }
};
enum { kDeviceStateIdle, kDeviceStateUpgrading };
struct Job {
    std::string state_="idle", ota="idle";
    std::mutex stream_mutex_, state_mutex_;
    std::atomic<bool> busy_{false}, ota_reserved_{false}, preview_worker_active_{false},
                      abort_reset_worker_active_{false}, paper_active_{false};
    struct Owner { bool Running() { return false; } } abort_reset_owner_;
    static Job& GetInstance() { static Job j; return j; }
    std::string StatusJson() { return "{}"; }
    void SetOtaStatus(const std::string& s, int, std::string) { ota=s; }
    void SetOtaUpdateAvailable(bool) {}
    bool TryReserveOta();
    bool OtaReservationActive();
    void ReleaseOtaReservation();
};
struct Application {
    int state=kDeviceStateIdle, upgrades=0;
    bool saw_active_job=false, fail_schedule=false;
    std::vector<std::function<void()>> queue;
    static Application& GetInstance() { static Application a; return a; }
    int GetDeviceState() { return state; }
    void SetDeviceState(int s) { state=s; }
    void Schedule(std::function<void()> f) { if (fail_schedule) throw std::runtime_error("schedule"); queue.push_back(f); }
    bool UpgradeFirmware(std::string, std::string) {
        ++upgrades;
        saw_active_job = Job::GetInstance().state_ == "streaming";
        return false;
    }
};
struct cJSON { const char* valuestring; };
cJSON* cJSON_Parse(const char*) { return nullptr; }
cJSON* cJSON_GetObjectItem(cJSON*, const char*) { return nullptr; }
bool cJSON_IsString(cJSON*) { return false; }
void cJSON_Delete(cJSON*) {}
struct CheckResult { bool update_available=false; std::string current,latest,notes,reason; };
CheckResult RunOtaCheck(bool) { return {}; }
std::string MakeCheckJson(bool,std::string,std::string,std::string,std::string) { return "{}"; }
std::string MakeReasonJson(std::string s) { return s; }
bool HostAllowed(const std::string&) { return true; }
bool JobIsIdleForOta() { return Job::GetInstance().state_ == "idle"; }
@@METHODS@@
@@REGISTER@@


int main() {
    McpServer server;
    RegisterTools(server);
    PropertyList args{{"url",1,"https://example.invalid/fw.bin"}, {"version",1,"2"},
                      {"board",1,BOARD_NAME}, {"sha256",1,""}};
    auto& app = Application::GetInstance();
    auto& job = Job::GetInstance();
    auto start = [&] { return server.callbacks.at("hutuji.ota_start")(args); };
    assert(start() == "{\"ok\":true}");
    assert(start() == "busy" && app.queue.size() == 1);
    assert(job.busy_ && job.OtaReservationActive());
    app.queue.front()();
    assert(app.upgrades == 1 && !job.OtaReservationActive() && !job.busy_);
    assert(job.ota == "failed");
    // 调度失败也释放；新的升级可重试。
    app.fail_schedule = true;
    assert(start() == "check_failed" && !job.busy_);
    app.fail_schedule = false;
    job.busy_ = true;
    assert(start() == "busy");
    job.busy_ = false;
    job.preview_worker_active_ = true;
    assert(start() == "busy");
    job.preview_worker_active_ = false;
    job.state_ = "streaming";
    assert(start() == "busy");
    job.state_ = "idle";
    // 两线程同时申请，只有一个所有者。
    std::atomic<int> claims{0};
    std::thread a([&] { if (job.TryReserveOta()) ++claims; });
    std::thread b([&] { if (job.TryReserveOta()) ++claims; });
    a.join(); b.join();
    assert(claims == 1);
    job.ReleaseOtaReservation();
    app.queue.clear();
    assert(start() == "{\"ok\":true}");
    job.ReleaseOtaReservation();
    app.queue.front()();
    assert(app.upgrades == 1); // 执行点所有权已失效，不可刷写。
}
'''


class OtaReservationTest(unittest.TestCase):
    def test_real_reservation_and_scheduled_callback(self):
        base = ROOT / "main/boards/lichuang-dev"
        job = (base / "hutuji_job.cc").read_text(encoding="utf-8")
        ota = (base / "hutuji_ota.cc").read_text(encoding="utf-8")
        methods = "\n".join(function(job, name) for name in (
            "bool Job::TryReserveOta(", "bool Job::OtaReservationActive(", "void Job::ReleaseOtaReservation("))
        program = PROGRAM.replace("@@METHODS@@", methods).replace("@@REGISTER@@", function(ota, "void RegisterTools("))
        compile_run(program)

    def test_motion_controls_use_same_ota_owner(self):
        source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        for name in ("RequestAbort", "RequestPause", "RequestResume"):
            self.assertIn("ota_reserved_.load()", function(source, "std::string Job::" + name + "("))
        for name in ("StartDraw", "RequestRepeat", "RequestManualControl", "RequestSpeed", "RequestPenTest"):
            self.assertIn("busy_", function(source, "std::string Job::" + name + "("))

if __name__ == "__main__":
    unittest.main()
