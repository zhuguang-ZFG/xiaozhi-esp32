"""真实配对握手与授权状态机：终止应答不能泄漏到下一查询。"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(os.environ.get('XIAOZHI_ROOT', Path(__file__).resolve().parents[2]))
def function(text, signature):
    start = text.index(signature)
    left = text.index("{", start)
    depth, end = 1, left + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


pipe = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(encoding="utf-8")
identity = (ROOT / "main/boards/lichuang-dev/factory_identity.h").read_text(encoding="utf-8")
header = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.h").read_text(encoding="utf-8")
program = r'''
#ifdef _WIN32
#include <winsock2.h>
using suseconds_t=long;
constexpr int MSG_DONTWAIT=0;
#else
#include <sys/socket.h>
#include <sys/select.h>
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include "pipe_review.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portTICK_PERIOD_MS 1
constexpr size_t kRxLineMax=512;
constexpr char kGrblBanner[]="Grbl ";
constexpr int kAuthProbeMaxRetries=8,kAuthProbeRetryDelayMs=5000;
std::deque<std::string> incoming;
std::vector<std::string> sent;
int query_count=0; bool paired=true,valid=true; int send_size=3;
int fake_select(int,fd_set*,fd_set*,fd_set*,timeval*){return incoming.empty()?0:1;}
int fake_recv(int,unsigned char* out,size_t max,int){auto& s=incoming.front();size_t n=std::min(max,s.size());std::memcpy(out,s.data(),n);s.erase(0,n);if(s.empty())incoming.pop_front();return (int)n;}
int fake_send(int,const char*,size_t n,int){++query_count;return send_size;}
int64_t esp_timer_get_time(){return 0;}
namespace hutuji {
struct FactoryIdentity { bool present=true,valid=true;std::string grbl="02:00:00:00:00:02"; };
FactoryIdentity LoadFactoryIdentity(){FactoryIdentity i;i.present=paired;i.valid=valid;return i;}
bool Pipe::SendRawLocked(const char* data,size_t size,bool){sent.emplace_back(data,size);return true;}
bool Pipe::SendRealtime(char value){sent.emplace_back(1,value);return true;}
void Pipe::DrainResponses(){}
void Pipe::PushResponse(WaitResult,int){}
void Pipe::NotifyCloud(const std::string&){}
'''
for signature in ("inline bool ParseFactoryMac(", "inline bool FactoryPeerMatches("):
    program += function(identity, signature) + "\n"
verify = function(pipe, "Pipe::PeerCheck Pipe::VerifyGrblPeer(")
program += verify.replace("select(", "fake_select(").replace("recv(", "fake_recv(").replace("send(", "fake_send(") + "\n"
for signature in (
    "int Pipe::ParseErrorCode(", "void Pipe::ProcessLine(", "void Pipe::ParseStatusReport(",
    "bool Pipe::HandleAuthProbeResponse(", "void Pipe::ResetSettingsFingerprintState(",
    "void Pipe::RecordSettingsMismatch(", "bool Pipe::BeginSettingsFingerprintProbe(",
    "void Pipe::ScheduleAuthProbeRetryOrFail(", "std::string Pipe::GetSettingsMismatchKey(",
    "bool Pipe::SendLine(", "bool Pipe::SendLineLocked(",
):
    program += function(pipe, signature) + "\n"
program += r'''
}
int main(int argc,char** argv){
 using namespace hutuji;
 const int scenario=std::stoi(argv[1]);
 const std::string banner="\r\nGrbl 1.3a ['$' for help]\r\n";
 const std::string mac="[MSG:Mode=STA:SSID=home:Status=Connected:IP=192.168.1.8:MAC=02-00-00-00-00-02]\r\n";
 incoming={banner};
 std::string reply="[VER:1.3a.20260910:]\r\n"+mac+"[MSG:No BT]\r\nok\r\n";
 if(scenario==0)incoming.push_back(reply);
 if(scenario==1){incoming.push_back(mac);incoming.push_back("ok\r\n");}
 if(scenario==2)for(char c:reply)incoming.emplace_back(1,c);
 if(scenario==3)incoming.push_back(mac);
 if(scenario==4)incoming.push_back(mac+"error:8\r\n");
 if(scenario==5)incoming.push_back(mac+"o");
 if(scenario==6)incoming.push_back("ok\r\n"+mac);
 if(scenario==7){reply.replace(reply.find("00-02"),5,"00-09");incoming.push_back(reply);}
 if(scenario==8){for(int i=0;i<20;++i)incoming.push_back("[MSG:Some build information]\r\n");incoming.push_back(reply);}
 if(scenario==9)incoming.push_back(std::string(513,'x')+"\n"+reply);
 if(scenario==10)paired=false;
 if(scenario==11)valid=false;
 if(scenario==12){send_size=2;incoming.push_back(reply);}
 if(scenario==13)incoming.push_back(mac+"Grbl 1.3a reset\r\nok\r\n");
 if(scenario==15)incoming.push_back("");
 if(scenario==14)incoming.push_back(mac+"ALARM:1\r\nok\r\n");
 Pipe p;
 const bool success=scenario<=2||scenario==8||scenario==10;
 const auto result=p.VerifyGrblPeer(1,1500);
 assert((result==Pipe::PeerCheck::Valid)==success);
 if(scenario==15)assert(result==Pipe::PeerCheck::Invalid);
 if(scenario==10){assert(query_count==0);return 0;}
 if(!success)return 0;
 assert(query_count==1&&incoming.empty());
 p.connected_=true;p.auth_probe_stage_=Pipe::AuthProbeStage::WaitingBuildInfoOk;
 sent.clear();p.SendRawLocked("$I\n",3);
 p.ProcessLine("[VER:1.3a.20260910:]",2);p.ProcessLine("ok",2);
 p.ProcessLine("ok",3);
 assert(p.auth_probe_stage_==Pipe::AuthProbeStage::WaitingPosition);
 p.ProcessLine("<Idle|MPos:0.000,0.000,0.000>",4);
 assert(p.auth_probe_stage_==Pipe::AuthProbeStage::WaitingMotionReply);
 p.ProcessLine("ok",5);
 assert(p.authorized_&&!p.ready_);
 p.ProcessLine("$1=255",6);p.ProcessLine("ok",6);
 assert(p.settings_query_index_==1&&!p.ready_);
}
'''

class FactoryHandshakeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
        path=Path(cls.temp.name);(path/'freertos').mkdir()
        (path/'pipe_review.h').write_text(header.replace('private:', 'public:'),encoding='utf-8')
        (path/'freertos/FreeRTOS.h').write_text('#pragma once\n#include <cstdint>\nusing TickType_t=uint32_t;using TaskHandle_t=void*;using QueueHandle_t=void*;inline uint32_t xTaskGetTickCount(){return 100;}\n#define pdMS_TO_TICKS(x) (x)\n')
        for name in ('queue.h','task.h'):(path/'freertos'/name).write_text('#include "FreeRTOS.h"\n')
        cpp=path/'probe.cpp';cpp.write_text(program,encoding='utf-8');cls.exe=path/'probe.exe'
        compiler=os.environ.get('CXX') or shutil.which('g++') or 'D:/zhugu-home/mingw64/mingw64/bin/g++.exe';cls.env=dict(os.environ,PATH=str(Path(compiler).parent)+os.pathsep+os.environ.get('PATH',''))
        result=subprocess.run([compiler,'-std=c++17','-Wno-narrowing','-I',str(path),'-I',str(ROOT/'main/boards/lichuang-dev'),str(cpp),'-o',str(cls.exe)],capture_output=True,env=cls.env,timeout=60)
        assert result.returncode==0,result.stderr.decode('utf-8','replace')
    def check(self,case):
        result=subprocess.run([str(self.exe),str(case)],capture_output=True,env=self.env,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr.decode('utf-8','replace'))

def case_method(n):
    def run(self):self.check(n)
    return run
for n,label in enumerate(('coalesced','split_ok','bytewise','missing_ok','error','partial_ok','early_ok','wrong_mac','long_report','oversized_line','legacy','bad_identity','partial_send','reset','alarm','close_after_banner')):
    setattr(FactoryHandshakeTest,'test_'+label,case_method(n))
