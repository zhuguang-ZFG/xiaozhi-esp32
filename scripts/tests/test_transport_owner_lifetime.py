"""编译完整 HTTP/WebSocket 实现，验证共享接收状态与迟到回调；不调用网络。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = Path(os.environ.get('TRANSPORT_COMPONENT', ROOT / 'managed_components/78__esp-ml307'))

FREERTOS = r'''
#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
using EventBits_t=unsigned;
struct Event{std::atomic<EventBits_t> bits{0};std::atomic<bool> alive{true};};
using EventGroupHandle_t=Event*;
inline std::atomic<int> events_live{0};
inline Event* xEventGroupCreate(){++events_live;return new Event;}
inline void vEventGroupDelete(Event* p){assert(p->alive.exchange(false));--events_live;}
inline unsigned xEventGroupClearBits(Event* p,unsigned bits){assert(p->alive);return p->bits.fetch_and(~bits);}
inline unsigned xEventGroupSetBits(Event* p,unsigned bits){assert(p->alive);return p->bits.fetch_or(bits);}
inline unsigned xEventGroupWaitBits(Event* p,unsigned,int,int,int){assert(p->alive);return p->bits.load();}
inline unsigned xTaskGetTickCount(){return 0;}
#define BIT0 1
#define BIT1 2
#define pdFALSE 0
#define pdMS_TO_TICKS(x) x
#define portTICK_PERIOD_MS 1
'''
NETWORK = r'''
#pragma once
#include "tcp.h"
#include <memory>
#include <vector>
#include <atomic>
inline std::atomic<int> transport_destroys{0},disconnects{0},before_wait{0};
inline void TestBeforeWait(){++before_wait;}
struct FakeTcp:Tcp{
 bool abandoned=false,fail_connect=false,fail_handshake=false,partial_http=false;
 int requests=0;
 ~FakeTcp(){++transport_destroys;}
 bool Connect(const std::string&,int)override{connected_=!fail_connect;return connected_;}
 void Disconnect()override{connected_=false;++disconnects;}
 bool ReceiveAbandoned()const override{return abandoned;}
 int GetLastError()override{return fail_connect?-7:0;}
 int Send(const std::string& data)override{
  if(data.rfind("GET ",0)==0||data.rfind("POST ",0)==0){
   ++requests;
   if(data.find("Upgrade: websocket")!=std::string::npos){
    stream_callback_(fail_handshake?"HTTP/1.1 400 Bad\r\n\r\n":"HTTP/1.1 101 Switching Protocols\r\n\r\n");
   }else if(partial_http) stream_callback_("HTTP/1.1 200 OK\r\nContent-Length: 12000\r\n\r\n"+std::string(6000,'A'));
   else stream_callback_("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\nok"+std::to_string(requests));
  }
  return static_cast<int>(data.size());
 }
 auto stream()const{return stream_callback_;}
 auto disconnected()const{return disconnect_callback_;}
};
struct NetworkInterface{
 bool abandon=false,fail_connect=false,fail_handshake=false,partial_http=false;
 std::vector<FakeTcp*> transports;
 std::unique_ptr<Tcp> CreateSsl(int){auto p=std::make_unique<FakeTcp>();p->abandoned=abandon;p->fail_connect=fail_connect;p->fail_handshake=fail_handshake;p->partial_http=partial_http;transports.push_back(p.get());return p;}
 std::unique_ptr<Tcp> CreateTcp(int n){return CreateSsl(n);}
};
'''
PROGRAM = r'''
#include "http_client.h"
#include "web_socket.h"
#include "network_interface.h"
#include <cassert>
#include <chrono>
#include <future>
#include <thread>
using namespace std::chrono_literals;
std::string frame(){return std::string("\x81\x02ok",4);}
std::string read(HttpClient& h){char buf[10];int n=h.Read(buf,sizeof(buf));assert(n>=0);return std::string(buf,n);}
int main(int argc,char**argv){
 int mode=std::atoi(argv[1]);NetworkInterface network;
 if(mode<=4||mode==10){
  network.abandon=mode==1||mode==2||mode==4;
  network.fail_connect=mode==10;
  auto h=std::make_unique<HttpClient>(&network);h->SetKeepAlive(true);
  if(mode==10){assert(!h->Open("GET","https://a.invalid/1"));h.reset();assert(events_live==0&&disconnects>=1);return 0;}
  assert(h->Open("GET","https://a.invalid/1"));assert(h->GetStatusCode()==200);assert(read(*h)=="ok1");
  auto old_stream=network.transports[0]->stream();auto old_disconnect=network.transports[0]->disconnected();
  if(mode==0){assert(h->Open("GET","https://a.invalid/2"));assert(network.transports.size()==1);assert(read(*h)=="ok2");}
  if(mode==1){h.reset();assert(transport_destroys==0&&events_live==0);old_stream("old");old_disconnect();assert(events_live==0);return 0;}
  if(mode==2){h->Close();assert(h->Open("GET","https://a.invalid/2"));old_disconnect();old_stream("garbage");assert(h->GetStatusCode()==200&&read(*h)=="ok1");assert(network.transports.size()==2);}
  if(mode==3){assert(h->Open("GET","https://b.invalid/2"));assert(network.transports.size()==2);assert(read(*h)=="ok1");assert(transport_destroys==1);}
  if(mode==4){
   // 新请求故意只给头部及部分大正文，再让第二段卡在真实 8KiB 背压等待中。
   h->Close();network.partial_http=true;before_wait=0;
   assert(h->Open("GET","https://a.invalid/3"));
   auto stream=network.transports.back()->stream();
   auto worker=std::async(std::launch::async,[stream]{stream(std::string(6000,'B'));});
   const auto deadline=std::chrono::steady_clock::now()+2s;
   while(before_wait<2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
   assert(before_wait==2);
   h.reset();assert(worker.wait_for(2s)==std::future_status::ready);worker.get();assert(events_live==0);return 0;
  }
  h.reset();assert(events_live==0);return 0;
 }
 network.abandon=mode!=5&&mode!=11;
 network.fail_handshake=mode==9;
 auto ws=std::make_unique<WebSocket>(&network,1);
 if(mode==11){ws.reset();assert(events_live==0&&network.transports.empty());return 0;}
 int received=0,disconnected=0;
 ws->OnData([&](const char*,size_t,bool){++received;});ws->OnDisconnected([&]{++disconnected;});
 bool connected=ws->Connect("wss://a.invalid/ws");
 if(mode==9){assert(!connected);ws.reset();assert(events_live==0&&transport_destroys==0&&disconnects>=1);return 0;}
 assert(connected);
 auto stream=network.transports[0]->stream();auto lost=network.transports[0]->disconnected();
 if(mode==5){stream(frame());assert(received==1);ws.reset();assert(transport_destroys==1&&events_live==0);return 0;}
 if(mode==6){ws.reset();stream(frame());lost();assert(received==0&&disconnected==0&&transport_destroys==0&&events_live==0);return 0;}
 if(mode==7){
  std::promise<void> entered,release;auto ready=release.get_future();
  ws->OnData([&](const char*,size_t,bool){entered.set_value();assert(ready.wait_for(2s)==std::future_status::ready);});
  auto worker=std::async(std::launch::async,[stream]{stream(frame());});
  assert(entered.get_future().wait_for(2s)==std::future_status::ready);
  ws.reset();assert(events_live==1&&transport_destroys==0);
  release.set_value();worker.get();assert(events_live==0&&transport_destroys==0);return 0;
 }
 assert(mode==8);assert(ws->Connect("wss://b.invalid/ws"));
 lost();stream(frame());assert(ws->IsConnected()&&received==0&&disconnected==0);
 network.transports.back()->stream()(frame());assert(received==1);
 ws.reset();assert(events_live==0&&transport_destroys==0);
}
'''


class TransportOwnerLifetimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('CXX') or shutil.which('g++')
        if not compiler or not COMPONENT.exists():
            raise unittest.SkipTest('需要 host C++ 编译器及已应用补丁的 esp-ml307 组件')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        (folder / 'freertos').mkdir()
        (folder / 'freertos/FreeRTOS.h').write_text(FREERTOS, encoding='utf-8')
        (folder / 'freertos/event_groups.h').write_text('#include "FreeRTOS.h"\n', encoding='utf-8')
        (folder / 'network_interface.h').write_text(NETWORK, encoding='utf-8')
        (folder / 'esp_log.h').write_text('\n'.join('#define ESP_LOG'+level+'(...) do{}while(0)' for level in 'EWI DV'.replace(' ','')), encoding='utf-8')
        (folder / 'esp_pthread.h').write_text('', encoding='utf-8')
        (folder / 'probe.cpp').write_text(PROGRAM, encoding='utf-8')
        # 只插入等待点计数器，原条件变量及队列逻辑保持真实实现。
        http = (COMPONENT / 'src/http_client.cc').read_text(encoding='utf-8')
        assert 'write_cv_.wait(' in http
        (folder / 'http_client.cc').write_text(http.replace('write_cv_.wait(', 'TestBeforeWait(); write_cv_.wait('), encoding='utf-8')
        cls.exe = folder / ('probe.exe' if os.name == 'nt' else 'probe')
        cls.env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get('PATH', ''))
        command = [compiler, '-std=c++17', '-O0', '-pthread', '-I'+str(folder), '-I'+str(COMPONENT/'include'),
                   str(folder/'probe.cpp'), str(folder/'http_client.cc'), str(COMPONENT/'src/web_socket.cc'), '-o', str(cls.exe)]
        built = subprocess.run(command, env=cls.env, capture_output=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stderr.decode('utf-8', 'replace'))

    def check(self, mode):
        ran = subprocess.run([str(self.exe), str(mode)], env=self.env, capture_output=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stderr.decode('utf-8', 'replace'))

    def test_http_keepalive(self): self.check(0)
    def test_http_abandoned_callbacks_after_destruction(self): self.check(1)
    def test_http_reopen_isolates_old_callbacks(self): self.check(2)
    def test_http_origin_change_reconnects(self): self.check(3)
    def test_http_receive_during_destruction(self): self.check(4)
    def test_websocket_normal_cleanup(self): self.check(5)
    def test_websocket_abandoned_callbacks_after_destruction(self): self.check(6)
    def test_websocket_inflight_callback_retains_state(self): self.check(7)
    def test_websocket_reconnect_isolates_old_callbacks(self): self.check(8)
    def test_websocket_handshake_failure_cleanup(self): self.check(9)
    def test_http_connect_failure_cleanup(self): self.check(10)
    def test_websocket_unconnected_cleanup(self): self.check(11)


if __name__ == '__main__':
    unittest.main()
