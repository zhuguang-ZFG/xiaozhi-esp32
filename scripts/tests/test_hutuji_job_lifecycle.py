"""用真实入口和任务生命周期验证忙拒绝、异步调速及重画通知，不计源码文案次数。"""
from pathlib import Path

from test_hutuji_ota_reservation import compile_run, function

ROOT = Path(__file__).resolve().parents[2]


def test_speed_accepts_before_device_io_and_busy_rejects():
    source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
    methods = "\n".join(function(source, name) for name in (
        "std::string Job::RequestSpeed(", "void Job::SpeedTaskEntry(", "void Job::RunSpeedUpdate("))
    compile_run(r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "main/boards/lichuang-dev/hutuji_speed_core.h"
using namespace hutuji;
using TickType_t=unsigned;
using BaseType_t=int;
constexpr int pdTRUE=1;
constexpr unsigned kSpeedWriteTimeoutMs=5000,kJogFreshStateTimeoutMs=6000;
unsigned ticks=0;
unsigned xTaskGetTickCount(){return ticks;}
unsigned pdMS_TO_TICKS(unsigned n){return n;}
void vTaskDelay(unsigned n){ticks+=n;}
void vTaskDelete(void*){}
unsigned esp_random(){return 42;}
void (*scheduled)(void*)=nullptr;
void* argument=nullptr;
bool worker_running=false;
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* arg,int,void*){
    assert(!scheduled);scheduled=fn;argument=arg;return pdTRUE;
}
enum class WaitResult{Ok,Timeout,Failed};
enum class GrblState{Idle};
struct Pipe {
    bool active=false;
    unsigned status=0;
    std::array<double,3> physical{6000,6000,1200};
    MachineSpeedSnapshot observed;
    std::vector<std::string> lines;
    static Pipe& GetInstance(){static Pipe p;return p;}
    bool IsConnected(){return true;}bool IsReady(){return true;}bool IsAuthorized(){return true;}
    bool IsSettingsVerified(){return true;}bool IsNopaperMachine(){return true;}
    unsigned GetConnectionSequence(){return 7;}unsigned GetResetBannerSequence(){return 9;}
    unsigned GetStatusReportSequence(){return status;}GrblState GetGrblState(){return GrblState::Idle;}
    void SetTaskSessionActive(bool value){active=value;}
    bool SendLineForSession(const char* raw,unsigned c,unsigned b){
        assert(worker_running&&c==7&&b==9);
        std::string line(raw);lines.push_back(line);
        const int setting=std::stoi(line.substr(1));const int index=setting-110;
        assert(index>=0&&index<3);
        const auto equals=line.find('=');
        if(equals!=std::string::npos)physical[index]=std::stod(line.substr(equals+1));
        else observed.Observe(std::to_string(setting),physical[index]);
        return true;
    }
    bool SendRealtime(char c){assert(worker_running&&c=='?');++status;return true;}
    WaitResult WaitResponse(unsigned){assert(worker_running);return WaitResult::Ok;}
    MachineSpeedSnapshot GetMachineSpeedSnapshot(){return observed;}
    void ShutdownSocket(unsigned){assert(false);}
};
struct Job {
    std::mutex stream_mutex_,state_mutex_;
    std::atomic<bool> busy_{false},preview_worker_active_{false},speed_active_{false};
    std::string state_="done",speed_state_,speed_reason_,speed_axis_;
    unsigned speed_request_id_=0,speed_connection_seq_=0,speed_banner_seq_=0;
    int speed_requested_rate_=0,speed_applied_rate_=0;
    MachineSpeedSnapshot speed_readback_;
    void StartPerformanceHold(){}void StopPerformanceHold(){}
    std::string RequestSpeed(int,const std::string&);
    static void SpeedTaskEntry(void*);
    void RunSpeedUpdate();
};
''' + methods + r'''
int main(){
    Job job;auto& pipe=Pipe::GetInstance();
    job.busy_=true;
    const auto rejected=job.RequestSpeed(7000,"x");
    assert(rejected.find("{\"error\":")==0&&job.busy_&&!scheduled&&pipe.lines.empty());
    job.busy_=false;
    const auto accepted=job.RequestSpeed(7000,"x");
    assert(accepted.find("\"status\":\"accepted\"")!=std::string::npos);
    assert(job.busy_&&job.speed_active_&&job.speed_state_=="pending"&&pipe.active);
    assert(scheduled&&argument==&job&&pipe.lines.empty()&&pipe.status==0);
    worker_running=true;scheduled(argument);
    assert(job.speed_state_=="done"&&job.speed_applied_rate_==7000);
    assert((pipe.physical==std::array<double,3>{7000,6000,1200}));
    assert((pipe.lines==std::vector<std::string>{"$110=7000","$110","$111","$112"}));
    assert(!job.busy_&&!job.speed_active_&&!pipe.active);
}
''')


def test_repeat_notifications_follow_drawing_outcome_and_keep_replay():
    source = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
    methods = "\n".join(function(source, name) for name in (
        "std::string Job::RequestRepeat(", "void Job::TaskEntry(", "void Job::Run("))
    compile_run(r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"
using namespace hutuji;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(n) (n)
using BaseType_t=int;
constexpr int pdTRUE=1,kDisconnectReplayMaxRetries=1;
constexpr unsigned kJobIdleTimeoutMs=60000;
void vTaskDelay(unsigned){}void vTaskDelete(void*){}
void (*scheduled)(void*)=nullptr;void* argument=nullptr;
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* arg,int,void*){
    assert(!scheduled);scheduled=fn;argument=arg;return pdTRUE;
}
struct Display{void SetStatus(const char*){}};
struct Board{static Board& GetInstance(){static Board b;return b;}Display* GetDisplay(){return nullptr;}};
struct SystemInfo{static void LogHeapNow(const char*){}};
struct PaperConfig{};
std::string PaperMarker(const PaperConfig&){return "paper";}
std::string JsonString(const char* s){return "\""+std::string(s)+"\"";}
enum class GrblState{Idle};
struct Pipe{
    bool active=false;
    static Pipe& GetInstance(){static Pipe p;return p;}
    bool IsConnected(){return true;}bool IsReady(){return true;}bool IsAuthorized(){return true;}
    bool IsSettingsVerified(){return true;}bool IsNopaperMachine(){return true;}
    unsigned GetConnectionSequence(){return 7;}unsigned GetResetBannerSequence(){return 9;}
    GrblState GetGrblState(){return GrblState::Idle;}
    void SetTaskSessionActive(bool value){active=value;}void SetExpectBlockingPeer(bool){}
};
struct Job{
    std::atomic<AbortDrainPhase> abort_drain_phase_{AbortDrainPhase::Idle};
    std::mutex stream_mutex_,state_mutex_;
    std::atomic<bool> busy_{false},preview_worker_active_{false},buffer_replayable_{true},
        abort_hold_confirmed_{false},abort_requested_{false},paused_{false},paper_active_{false},
        repeat_mode_{false},stream_error_stop_required_{false},abort_reset_worker_active_{false},abort_home_done_{false};
    std::atomic<StreamQuiescence> stream_quiescence_{StreamQuiescence::Idle};
    struct Owner{bool Running(){return false;}bool Started(){return false;}bool Succeeded(){return true;}}abort_reset_owner_;
    std::string state_="done",url_,last_error_,job_paper_marker_="paper",buffer_paper_marker_="paper";
    std::vector<std::pair<std::string,std::string>> article_pages_;
    size_t article_index_=0;
    std::atomic<unsigned> article_page_{0};
    char cached='x';char* buffer_=&cached;size_t buffer_len_=1;
    unsigned stream_connection_seq_=0,stream_banner_seq_=0;
    bool paper_change_notified_=false,failure_notified_=false,stream_disconnected_=false,finishing_at_home_=false;
    bool stream_ok=true,homed=false,released=false;
    int downloads=0,streams=0;
    std::vector<std::pair<std::string,bool>> notifications;
    void Notify(const std::string&){notifications.emplace_back(state_,released);}
    bool GetPaperConfig(PaperConfig&){return true;}bool ResetAbortResetState(){return true;}
    void ReleaseBuffer(){buffer_=nullptr;buffer_len_=0;}
    void ReleaseFetchClient(){}void SetState(const char* s){state_=s;}
    bool AdoptPrefetch(){return false;}bool DownloadToPsram(const std::string&){++downloads;return true;}
    bool VerifyCrc(){return true;}void SetStreamingOrPaused(){state_=paused_?"paused":"streaming";}
    bool PreparePenOrigin(){return true;}
    bool StreamToGrbl(){++streams;if(!stream_ok)last_error_="test transport failure";return stream_ok;}
    bool WaitForIdle(bool,unsigned){return true;}bool RecoverDisconnectedDraw(){return false;}
    bool PerformAbortReset(bool){return false;}
    bool ReturnHomeAfterDraw(){homed=true;return true;}bool ChangePaperAfterDraw(){assert(false);return false;}
    bool ReleaseMotorsAfterHome(unsigned,unsigned,bool){assert(homed);released=true;return true;}
    bool WaitForAbortReset(){return true;}
    std::string RequestRepeat();static void TaskEntry(void*);void Run();
};
''' + methods + r'''
int main(){
    for(bool succeeds:{true,false}){
        scheduled=nullptr;argument=nullptr;
        Job job;job.stream_ok=succeeds;
        job.busy_=true;
        const auto rejected=job.RequestRepeat();
        assert(rejected.find("{\"error\":")==0&&job.busy_&&!scheduled);
        job.busy_=false;
        assert(job.RequestRepeat()==JsonString("started"));
        assert(job.busy_&&job.repeat_mode_&&scheduled&&argument==&job);
        scheduled(argument);
        assert(job.downloads==0&&job.streams==1&&!job.busy_&&!Pipe::GetInstance().active);
        assert(job.notifications.size()==2);
        assert(job.notifications[0]==std::make_pair(std::string("streaming"),false));
        if(succeeds){
            assert(job.state_=="done"&&job.homed&&job.released&&job.buffer_replayable_&&job.buffer_);
            assert(job.notifications[1]==std::make_pair(std::string("done"),true));
        }else{
            assert(job.state_=="error"&&!job.homed&&!job.released&&!job.buffer_replayable_&&!job.buffer_);
            assert(job.notifications[1]==std::make_pair(std::string("error"),false));
        }
    }
}
''')
