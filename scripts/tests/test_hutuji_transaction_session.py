"""真实归位/调速与Pipe发送：会话变化前后都不向新会话写动作。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

def function(source, signature):
    start=source.index(signature);left=source.index("{",start);right=left+1;depth=1
    while depth:
        depth+=(source[right]=="{")-(source[right]=="}");right+=1
    return source[start:right]

ROOT=Path(os.environ.get('XIAOZHI_ROOT',Path(__file__).resolve().parents[2]))
PROGRAM=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "@@CORE@@"
#include "@@SPEED@@"
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
    WaitResult WaitResponse(uint32_t,void* = nullptr,int* = nullptr){return scenario==8?WaitResult::Timeout:scenario==9?WaitResult::Error:WaitResult::Ok;}
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
    bool SendLineLocked(const std::string&);
    bool SendLineForSession(const std::string&,uint32_t,uint32_t);
    bool SendResumeForSession(uint32_t,uint32_t);
    bool IsCommandSessionCurrent(uint32_t,uint32_t) const;
};
void vTaskDelay(uint32_t ms){
    clock_ms+=ms;
    if(ms==kPenSpringReturnMs && !switched){
        switched=true;auto& p=Pipe::GetInstance();
        if(scenario==1||scenario==4){++p.connection_seq_;++p.reset_banner_seq_;}
        if(scenario==2||scenario==5)++p.reset_banner_seq_;
        // 与真实reset banner处理一致：活动任务内不会重新自动授权。
        if(scenario==1||scenario==2){p.ready_=false;p.authorized_=false;p.settings_verified_=false;}
        if(scenario==3)p.connected_=false;
    }
}
void invalidate_writer(){
    if(switched)return;
    if((test_mode==2&&(scenario==1||scenario==2))||scenario==6||scenario==7){
        switched=true;auto& p=Pipe::GetInstance();
        if(scenario==1||scenario==6)++p.connection_seq_;else ++p.reset_banner_seq_;
        // 保持ready，单纯增加事后ready判断不能通过此用例。
    }
}
struct Job {
    std::mutex state_mutex_;
    int speed_requested_rate_=7000,speed_applied_rate_=0;
    std::string speed_axis_="xy",speed_state_="pending",speed_reason_;
    uint32_t speed_connection_seq_=7,speed_banner_seq_=9;
    std::atomic<bool> speed_active_{true},busy_{true};
    MachineSpeedSnapshot speed_readback_;
    void StartPerformanceHold(){}void StopPerformanceHold(){}
    void RunSpeedUpdate();

    std::mutex stream_mutex_;
    std::atomic<bool> abort_requested_{false},paused_{false},abort_home_done_{false};
    std::atomic<StreamQuiescence> stream_quiescence_{StreamQuiescence::Quiesced};
    uint32_t stream_connection_seq_=7;
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
@@METHODS@@

int main(int argc,char**argv){
    test_mode=std::stoi(argv[1]);scenario=std::stoi(argv[2]);Job job;auto& pipe=Pipe::GetInstance();
    if(test_mode==3){
        const bool result=pipe.SendResumeForSession(7,9);
        assert(result==(scenario==0));
        assert(pipe.commands.size()==(scenario==0?1u:0u));
        if(result)assert(pipe.commands[0]=="~");
    }else if(test_mode==2){
        if(scenario==3)++pipe.reset_banner_seq_;
        if(scenario==4)++pipe.connection_seq_;
        job.RunSpeedUpdate();
        if(scenario==0){assert(job.speed_state_=="done"&&pipe.speed.rates[0]==7000&&pipe.speed.rates[1]==7000);}
        else {assert(job.speed_state_=="error"&&pipe.speed.rates[0]==10000&&pipe.speed.rates[1]==10000);assert(pipe.commands.empty());}
        assert(!job.busy_&&!job.speed_active_);
    }else{
        const bool result=test_mode==0?job.ReturnHomeAfterDraw():job.PerformAbortDrainHome();
        const bool homed=test_mode==0?result:job.abort_home_done_.load();
        assert(homed==(scenario==0));
        assert(pipe.new_writes==0);
        assert(pipe.commands.size()==(scenario==0?2u:(scenario==6||scenario==7)?0u:1u));
        if(scenario==8||scenario==9)assert(pipe.shutdowns==1);
        if(scenario==1||scenario==4||scenario==6)assert(pipe.shutdowns==0);
    }
}
'''

class TransactionSessionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=os.environ.get("CXX") or shutil.which("g++")
        if not compiler:raise unittest.SkipTest("需要host C++编译器")
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
        base=ROOT/"main/boards/lichuang-dev"
        pipe=(base/"hutuji_pipe.cc").read_text(encoding="utf-8")
        job=(base/"hutuji_job.cc").read_text(encoding="utf-8")
        names=["bool Pipe::SendLine(","bool Pipe::SendLineLocked(","bool Pipe::IsCommandSessionCurrent(","bool Pipe::SendLineForSession("]
        if "bool Pipe::SendResumeForSession(" in pipe:names.append("bool Pipe::SendResumeForSession(")
        methods="\n".join(function(pipe,name) for name in names)
        methods=methods.replace("std::lock_guard<std::mutex> lock(write_mutex_);","std::lock_guard<WriterMutex> lock(write_mutex_);")
        names=["bool Job::WaitForIdle(","bool Job::ReturnHomeAfterDraw(","bool Job::PerformAbortDrainHome(","void Job::RunSpeedUpdate("]
        if "bool Job::WaitForIdleForSession(" in job:names.append("bool Job::WaitForIdleForSession(")
        methods+="\n"+"\n".join(function(job,name) for name in names)
        program=PROGRAM.replace("@@CORE@@",(base/"hutuji_recovery_core.h").as_posix()).replace("@@SPEED@@",(base/"hutuji_speed_core.h").as_posix()).replace("@@METHODS@@",methods)
        source=Path(cls.temp.name)/"test.cpp";source.write_text(program,encoding="utf-8")
        cls.exe=source.with_suffix(".exe" if os.name=="nt" else "")
        cls.env=dict(os.environ,PATH=str(Path(compiler).parent)+os.pathsep+os.environ.get("PATH",""))
        built=subprocess.run([compiler,"-std=c++17","-O0",str(source),"-o",str(cls.exe)],capture_output=True,env=cls.env,timeout=60)
        if built.returncode:raise AssertionError(built.stderr.decode("utf-8","replace"))
    def check(self,mode,case):
        result=subprocess.run([str(self.exe),str(mode),str(case)],capture_output=True,env=self.env,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr.decode("utf-8","replace"))

def case_method(mode,case):
    def run(self):self.check(mode,case)
    return run

for mode,label in enumerate(("normal_home","abort_home")):
    for case,reason in enumerate(("success","reconnect","reset","offline","reconnect_ready","reset_ready","writer_reconnect","writer_reset","ack_timeout","ack_error")):
        setattr(TransactionSessionTest,"test_"+label+"_"+reason,case_method(mode,case))
for case,label in enumerate(("success","writer_reconnect","writer_reset","queued_reset","queued_reconnect")):
    setattr(TransactionSessionTest,"test_speed_"+label,case_method(2,case))
for case,label in ((0,"success"),(6,"reconnect"),(7,"reset")):
    setattr(TransactionSessionTest,"test_resume_"+label,case_method(3,case))

if __name__=="__main__":unittest.main()
