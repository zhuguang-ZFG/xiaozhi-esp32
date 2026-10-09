"""编译真实产品函数，检查输入/会话边界；仅外部I/O由宿主替身代替。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

def function(source, signature):
    start=source.index(signature); left=source.index('{',start); right=left+1; depth=1
    while depth:
        depth+=(source[right]=='{')-(source[right]=='}'); right+=1
    return source[start:right]
STUB=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "D:/Users/xiaozhi-esp32/main/boards/lichuang-dev/hutuji_recovery_core.h"
#include "D:/Users/xiaozhi-esp32/main/boards/lichuang-dev/hutuji_speed_core.h"
using namespace hutuji;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define HUTUJI_QUIET_STREAM_LOG 1
#define pdMS_TO_TICKS(x) (x)
using TickType_t=uint32_t;
constexpr uint32_t portTICK_PERIOD_MS=1;
constexpr uint32_t kPenSpringReturnMs=100, kHomeOkTimeoutMs=1000, kHomeIdleTimeoutMs=60000;
constexpr uint32_t kPenOriginIdleTimeoutMs=1000, kAbortDrainIdleTimeoutMs=15000, kAbortDrainQuiescenceTimeoutMs=5000;
uint32_t clock_ms=0;
int scenario=0;
bool switched=false;
TickType_t xTaskGetTickCount(){return clock_ms;}
void vTaskDelay(uint32_t);
enum class GrblState {Idle,Hold,Alarm};
enum class WaitResult {Ok,Timeout,Error};

constexpr uint32_t kPaperOkTimeoutMs=90000,kZ0WaitSliceMs=1000,kZ0QuietWaitMs=6000;
using BaseType_t=int;
constexpr int pdTRUE=1;
int lock_count=0;
int xTaskCreate(void(*fn)(void*),const char*,int,void* arg,int,void*){fn(arg);return pdTRUE;}
void vTaskDelete(void*){}
struct Display {void SetStatus(const char*){}};
struct Board {static Board& GetInstance(){static Board b;return b;}Display* GetDisplay(){return nullptr;}};
std::string JsonString(const char* value){return value;}
void invalidate_writer();
struct WriterMutex { std::mutex value;void lock(){invalidate_writer();value.lock();}void unlock(){value.unlock();} };
constexpr uint32_t kSpeedWriteTimeoutMs=5000,kJogFreshStateTimeoutMs=6000;
int test_mode=0;
struct Pipe {
    WriterMutex write_mutex_;
    std::atomic<bool> connected_{true},ready_{true},authorized_{true},settings_verified_{true},task_session_active_{true},drain_on_send_{true};
    std::atomic<uint32_t> connection_seq_{7},reset_banner_seq_{9};
    uint32_t status_seq=0;
    int old_writes=0,new_writes=0,shutdowns=0;
    MachineSpeedSnapshot speed{{10000,10000,1200},{0,0,0}};
    bool IsAuthorized(){return authorized_;}bool IsSettingsVerified(){return settings_verified_;}
    MachineSpeedSnapshot GetMachineSpeedSnapshot(){return speed;}
    void SetTaskSessionActive(bool active){task_session_active_=active;}
    void ShutdownSocket(uint32_t c){if(c==connection_seq_){++shutdowns;connected_=false;}}

    std::vector<std::string> commands;
    static Pipe& GetInstance(){static Pipe p;return p;}
    bool IsConnected(){return connected_;} bool IsReady(){return ready_;}
    bool IsNopaperMachine(){return true;}
    GrblState GetGrblState(){return GrblState::Idle;}
    uint32_t GetConnectionSequence(){return connection_seq_;}
    uint32_t GetResetBannerSequence(){return reset_banner_seq_;}
    uint32_t GetStatusReportSequence(){return status_seq;}
    int GetAlarmCode(){return 0;}
    bool SendRealtime(char c){if(c=='?')++status_seq;return connected_;}

WaitResult WaitResponse(uint32_t,void* = nullptr,int* = nullptr){
    if(scenario==8&&!switched){switched=true;++reset_banner_seq_;ready_=false;authorized_=false;settings_verified_=false;}
    return WaitResult::Ok;
}
WaitResult TakeResponse(uint32_t){return WaitResult::Ok;}
void SetWindowed(bool value){drain_on_send_=!value;}
void SetExpectBlockingPeer(bool){}

    void DrainResponses(){}
    bool SendRawLocked(const char* data,size_t size){
        if(!connected_)return false;
        (connection_seq_==7 && reset_banner_seq_==9 ? old_writes : new_writes)++;
        commands.emplace_back(data,size);
        int setting=0,rate=0;
        if(test_mode==2){
            if(std::sscanf(data,"$%d=%d",&setting,&rate)==2)speed.rates.at(setting-110)=rate;
            else if(std::sscanf(data,"$%d",&setting)==1)++speed.revisions.at(setting-110);
        }
        return true;
    }
    bool SendLine(const std::string&);
    bool SendLineForSession(const std::string&,uint32_t,uint32_t);
    bool IsCommandSessionCurrent(uint32_t,uint32_t) const;
    bool SendLineLocked(const std::string&);
    bool SendResumeForSession(uint32_t,uint32_t);

};
void vTaskDelay(uint32_t ms){clock_ms+=ms;}
void invalidate_writer(){
    ++lock_count;
    if(switched)return;
    bool target=(test_mode==4&&lock_count==1)||(test_mode==5&&lock_count==(scenario==9||scenario==10?3:2));
    if(target&&(scenario==6||scenario==7||scenario==9||scenario==10)){
        switched=true;auto& p=Pipe::GetInstance();
        if(scenario==6||scenario==9)++p.connection_seq_;else ++p.reset_banner_seq_;
        p.ready_=false;p.authorized_=false;p.settings_verified_=false;
    }
}
struct Job {
    static Job& GetInstance(){static Job job;return job;}
    std::atomic<bool> preview_worker_active_{false},pen_test_active_{false},abort_hold_confirmed_{false};
    bool paper_change_notified_=false;
    std::string state_="idle";
    void SetState(const std::string& s){state_=s;}
    void Notify(const std::string&){}
    bool PreparePenOrigin();
    std::string RequestPenTest();

    std::mutex state_mutex_;
    int speed_requested_rate_=7000,speed_applied_rate_=0;
    std::string speed_axis_="xy",speed_state_="pending",speed_reason_;
    uint32_t speed_connection_seq_=7,speed_banner_seq_=9;
    std::atomic<bool> speed_active_{false},busy_{false};
    MachineSpeedSnapshot speed_readback_;
    void StartPerformanceHold(){}void StopPerformanceHold(){}
    void RunSpeedUpdate();

    std::mutex stream_mutex_;
    std::atomic<bool> abort_requested_{false},paused_{false},abort_home_done_{false};
    std::atomic<StreamQuiescence> stream_quiescence_{StreamQuiescence::Quiesced};
    uint32_t stream_connection_seq_=7,stream_banner_seq_=9;
    bool stream_disconnected_=false;
    std::string last_error_;
    bool WaitWhilePaused(){return !abort_requested_;}
    void SetStreamingOrPaused(){}
    bool StartAbortResetTask(){return false;}
    bool ReleaseMotorsAfterHome(uint32_t c,uint32_t b,bool){auto& p=Pipe::GetInstance();return p.IsConnected()&&p.GetConnectionSequence()==c&&p.GetResetBannerSequence()==b;}
    bool WaitForIdle(bool,uint32_t);
    bool WaitForIdleForSession(bool,uint32_t,uint32_t,uint32_t);
    bool ReturnHomeAfterDraw();
    bool PerformAbortDrainHome();
};
'''
MAIN=r'''
int main(int argc,char**argv){
    test_mode=std::stoi(argv[1]);scenario=std::stoi(argv[2]);
    auto& j=Job::GetInstance();auto& p=Pipe::GetInstance();
    if(scenario==11){++p.reset_banner_seq_;p.ready_=false;}
    bool ok=test_mode==4?j.PreparePenOrigin():false;
    if(test_mode==5)j.RequestPenTest();
    assert(p.new_writes==0);
    if(test_mode==4){assert(ok==(scenario==0));}
    else {assert(j.state_==(scenario==0?"done":"error"));assert(!j.busy_&&!j.pen_test_active_&&!p.task_session_active_);}
    if(scenario==0)assert(p.commands.size()==(test_mode==4?1u:3u));
}
'''
def program():
    root=Path(os.environ.get('XIAOZHI_ROOT',Path(__file__).resolve().parents[2]))
    base=root/'main/boards/lichuang-dev'
    headers=Path(os.environ.get('XIAOZHI_HEADERS_ROOT',root))/'main/boards/lichuang-dev'
    stub=STUB
    import re
    stub=re.sub(r'#include "[^"]*hutuji_recovery_core.h"','#include "'+(headers/'hutuji_recovery_core.h').as_posix()+'"',stub)
    stub=re.sub(r'#include "[^"]*hutuji_speed_core.h"','#include "'+(headers/'hutuji_speed_core.h').as_posix()+'"',stub)
    pipe=(base/'hutuji_pipe.cc') if (base/'hutuji_pipe.cc').exists() else headers/'hutuji_pipe.cc'
    source=pipe.read_text(encoding='utf-8')
    methods='\n'.join(function(source,n) for n in ('bool Pipe::SendLine(', 'bool Pipe::SendLineLocked(', 'bool Pipe::SendLineForSession(', 'bool Pipe::IsCommandSessionCurrent('))
    methods=methods.replace('std::lock_guard<std::mutex> lock(write_mutex_);','std::lock_guard<WriterMutex> lock(write_mutex_);')
    job=(base/'hutuji_job.cc').read_text(encoding='utf-8')
    methods+='\n'+'\n'.join(function(job,n) for n in ('bool Job::WaitForIdle(', 'bool Job::WaitForIdleForSession(', 'bool Job::PreparePenOrigin(', 'std::string Job::RequestPenTest('))
    return stub+methods+MAIN

class RegressionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=os.environ.get('CXX') or shutil.which('g++')
        if not compiler:raise RuntimeError('需要host C++编译器')
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
        path=Path(cls.temp.name)/'case.cpp';path.write_text(program(),encoding='utf-8')
        cls.exe=path.with_suffix('.exe' if os.name=='nt' else '')
        cls.env=dict(os.environ,PATH=str(Path(compiler).parent)+os.pathsep+os.environ.get('PATH',''))
        built=subprocess.run([compiler,'-std=c++17','-O0',str(path),'-o',str(cls.exe)],env=cls.env,capture_output=True,timeout=60)
        if built.returncode:raise AssertionError(built.stderr.decode('utf-8','replace'))
    def check(self,*args):
        result=subprocess.run([str(self.exe),*map(str,args)],env=self.env,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr.decode('utf-8','replace'))

def case(mode,scenario):
    def run(self):self.check(mode,scenario)
    return run
for mode,label in ((4,'origin'),(5,'pen')):
    for scenario,reason in ((0,'success'),(6,'writer_reconnect'),(7,'writer_reset'),(8,'late_ack_reset')):
        setattr(RegressionTest,'test_'+label+'_'+reason,case(mode,scenario))
for scenario,reason in ((9,'second_writer_reconnect'),(10,'second_writer_reset')):
    setattr(RegressionTest,'test_pen_'+reason,case(5,scenario))
if __name__=='__main__':unittest.main()
