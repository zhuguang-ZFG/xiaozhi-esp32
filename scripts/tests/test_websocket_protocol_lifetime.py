"""编译完整 Protocol/WebsocketProtocol/WebSocket/cJSON，固定消费者寿命交错。"""
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = runpy.run_path(str(ROOT / 'scripts/tests/test_transport_owner_lifetime.py'))
COMPONENT = FIXTURE['COMPONENT']
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'


def replace_once(source, old, new):
    assert source.count(old) == 1, old
    return source.replace(old, new)


# 仅扩展宿主事件/网络边界；被测四份完整源码不抽取、不复制、不改写。
FREERTOS = replace_once(FIXTURE['FREERTOS'], '#include <cstdint>', '''#include <cstdint>
#include <functional>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>''')
FREERTOS = replace_once(FREERTOS, 'inline std::atomic<int> events_live{0};', '''inline std::atomic<int> events_live{0}, deleted_event_sets{0};
inline std::vector<Event*> created_events;
inline std::function<void(Event*)> after_event_delete;
inline std::function<void(Event*, unsigned)> before_event_wait;''')
FREERTOS = replace_once(FREERTOS, '++events_live;return new Event;', '++events_live;auto p=new Event;created_events.push_back(p);return p;')
FREERTOS = replace_once(FREERTOS, 'assert(p->alive.exchange(false));--events_live;', 'assert(p->alive.exchange(false));--events_live;if(after_event_delete)after_event_delete(p);')
FREERTOS = replace_once(FREERTOS, 'assert(p->alive);return p->bits.fetch_or(bits);', 'if(!p->alive){++deleted_event_sets;return 0;}return p->bits.fetch_or(bits);')
FREERTOS = replace_once(FREERTOS, 'inline unsigned xEventGroupWaitBits(Event* p,unsigned,int,int,int){assert(p->alive);return p->bits.load();}', '''inline unsigned xEventGroupWaitBits(Event* p,unsigned bits,int clear,int,int){
 if(before_event_wait)before_event_wait(p,bits);
 assert(p->alive);auto value=p->bits.load();if(clear)p->bits.fetch_and(~bits);return value;
}''') + '\n#define pdTRUE 1\n'
NETWORK = replace_once(FIXTURE['NETWORK'], '#include "tcp.h"', '#include "tcp.h"\n#include "web_socket.h"\n#include "wire.h"')
NETWORK = replace_once(NETWORK, 'void Disconnect()override{connected_=false;++disconnects;}', '''void Disconnect()override{
 if(!connected_.exchange(false))return;
 ++disconnects;if(disconnect_callback_)disconnect_callback_();
}''')
NETWORK = replace_once(NETWORK, 'int requests=0;', 'int requests=0;\n std::vector<std::string> sent;')
NETWORK = replace_once(NETWORK, 'if(data.rfind("GET ",0)==0||data.rfind("POST ",0)==0){', '''if(data.rfind("GET ",0)!=0 && data.rfind("POST ",0)!=0){
   sent.push_back(data);if(fail_send)return -1;
  }
  if(data.rfind("GET ",0)==0||data.rfind("POST ",0)==0){''')
NETWORK = replace_once(NETWORK, 'stream_callback_(fail_handshake?"HTTP/1.1 400 Bad\\r\\n\\r\\n":"HTTP/1.1 101 Switching Protocols\\r\\n\\r\\n");', '''std::string response=fail_handshake?"HTTP/1.1 400 Bad\\r\\n\\r\\n":"HTTP/1.1 101 Switching Protocols\\r\\n\\r\\n";
    if(auto_hello&&!fail_handshake)response+=frame(hello_payload);
    stream_callback_(response);''')
NETWORK = replace_once(NETWORK, 'std::unique_ptr<Tcp> CreateTcp(int n){return CreateSsl(n);}', '''std::unique_ptr<Tcp> CreateTcp(int n){return CreateSsl(n);}
 std::unique_ptr<WebSocket> CreateWebSocket(int n){
  if(fail_create)return nullptr;return std::make_unique<WebSocket>(this,n);
 }''')

WIRE = r'''
#pragma once
#include <string>
inline bool auto_hello=true,fail_send=false,fail_create=false;
inline int selected_version=1;
inline std::string hello_payload=R"({"type":"hello","transport":"websocket","session_id":"first","audio_params":{"sample_rate":16000,"frame_duration":40}})";
inline std::string frame(const std::string& data,bool binary=false){
 std::string out(1,binary?'\x82':'\x81');
 if(data.size()<126)out+=static_cast<char>(data.size());
 else{out+='\x7e';out+=static_cast<char>(data.size()>>8);out+=static_cast<char>(data.size());}
 return out+data;
}
inline std::string unmask(const std::string& data){
 size_t size=static_cast<unsigned char>(data[1])&127,offset=2;
 if(size==126){size=(static_cast<unsigned char>(data[2])<<8)|static_cast<unsigned char>(data[3]);offset=4;}
 std::string out;for(size_t i=0;i<size;++i)out+=data[offset+4+i]^data[offset+i%4];return out;
}
'''
CLOCK = r'''
#pragma once
#include <memory>
#include <cstdint>
#include <chrono>
#include <future>
#include <thread>
#include <atomic>
inline std::atomic<long long> clock_ms{500000};
namespace std::chrono {
struct FixtureClock {
 using duration=steady_clock::duration;using rep=duration::rep;using period=duration::period;
 using time_point=std::chrono::time_point<FixtureClock>;
 static constexpr bool is_steady=true;
 static time_point now(){return time_point(duration_cast<duration>(milliseconds(clock_ms.load())));}
};
}
#define steady_clock FixtureClock
'''
STUBS = {
    'application.h': '#pragma once\n#define OPUS_FRAME_DURATION_MS 60\n',
    'settings.h': '''#pragma once
#include "wire.h"
struct Settings {
 Settings(const char*,bool){}
 std::string GetString(const char* key){return std::string(key)=="url"?"wss://fixture.invalid/ws":"";}
 int GetInt(const char*){return selected_version;}
};
''',
    'system_info.h': '''#pragma once
#include <string>
struct SystemInfo{static std::string GetMacAddress(){return "fixture-device";}};
''',
    'board.h': '''#pragma once
#include "network_interface.h"
inline NetworkInterface* selected_network=nullptr;
struct Board{
 static Board& GetInstance(){static Board b;return b;}
 NetworkInterface* GetNetwork(){return selected_network;}
 std::string GetUuid(){return "fixture-client";}
};
''',
    'assets/lang_config.h': '''#pragma once
namespace Lang {namespace Strings {
inline const char* SERVER_ERROR="error";
inline const char* SERVER_NOT_CONNECTED="not_connected";
inline const char* SERVER_TIMEOUT="timeout";
}}
''',
    'assets.h': '''#pragma once
#include <string>
struct FontCaps{bool glyph_push=false;std::string bundle,charset;int size=0,bpp=0;};
struct Assets{
 static Assets& GetInstance(){static Assets a;return a;}
 FontCaps text_font_capability(){return {};}
};
''',
    'arpa/inet.h': '''#pragma once
#include <cstdint>
inline uint16_t htons(uint16_t n){return __builtin_bswap16(n);}
inline uint16_t ntohs(uint16_t n){return __builtin_bswap16(n);}
inline uint32_t htonl(uint32_t n){return __builtin_bswap32(n);}
inline uint32_t ntohl(uint32_t n){return __builtin_bswap32(n);}
''',
}
PROGRAM = r'''
#include "websocket_protocol.h"
#include "board.h"
#include "device_state_machine.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <vector>
using namespace std::chrono_literals;
struct Barrier{
 std::promise<void> entered,release;
 std::future<void> entry=entered.get_future(),resume=release.get_future();
 void pause(){entered.set_value();assert(resume.wait_for(2s)==std::future_status::ready);}
 void wait(){assert(entry.wait_for(2s)==std::future_status::ready);}
};
// 与 Application::Run 相同：整批移出主任务队列，执行期间新排队的任务留到下一批。
struct MainTasks{
 std::mutex mutex;std::vector<std::function<void()>> tasks;
 void schedule(std::function<void()> task){std::lock_guard<std::mutex> lock(mutex);tasks.push_back(std::move(task));}
 void run(){std::vector<std::function<void()>> batch;{std::lock_guard<std::mutex> lock(mutex);batch=std::move(tasks);}for(auto& task:batch)task();}
};
// cJSON 分配钩子在实际解析入口暂停，不改写被测源码。
inline thread_local Barrier* parse_barrier=nullptr;
void* json_alloc(size_t n){if(parse_barrier){auto b=parse_barrier;parse_barrier=nullptr;b->pause();}return std::malloc(n);}
struct Owner{
 alignas(WebsocketProtocol) unsigned char storage[sizeof(WebsocketProtocol)];
 unsigned char after[sizeof(WebsocketProtocol)];
 WebsocketProtocol* p=new(storage) WebsocketProtocol;
 void destroy(){if(p){auto old=p;p=nullptr;old->~WebsocketProtocol();memcpy(after,storage,sizeof storage);}}
 void unchanged(){assert(memcmp(after,storage,sizeof storage)==0);}
 ~Owner(){destroy();}
};
std::string audio(int version){
 if(version==2)return std::string("\0\2\0\0\0\0\0\0\x12\x34\x56\x78\0\0\0\3\x11\x22\x33",19);
 if(version==3)return std::string("\0\0\0\3\x11\x22\x33",7);
 return std::string("\x11\x22\x33",3);
}
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];
 NetworkInterface network;selected_network=&network;
 cJSON_Hooks hooks{json_alloc,std::free};cJSON_InitHooks(&hooks);
 Owner owner;int closed=0,opened=0,received=0,jsons=0,errors=0;
 bool low_power=false,idle=false;
 owner.p->OnAudioChannelClosed([&]{++closed;low_power=true;idle=true;});
 owner.p->OnAudioChannelOpened([&]{++opened;low_power=false;idle=false;});
 owner.p->OnNetworkError([&](const std::string&){++errors;});
 owner.p->OnIncomingAudio([&](std::unique_ptr<AudioStreamPacket> p){
  ++received;assert(p->payload==std::vector<uint8_t>({0x11,0x22,0x33}));
  assert(p->sample_rate==16000&&p->frame_duration==40);
  assert(p->timestamp==(selected_version==2?0x12345678u:0u));
 });
 owner.p->OnIncomingJson([&](const cJSON* root){++jsons;assert(cJSON_IsString(cJSON_GetObjectItem(root,"type")));});
 if(mode=="timeout"||mode=="waiter_destroy"||mode=="waiter_remote")auto_hello=false;
 if(mode=="connect_failure")network.fail_connect=true;
 if(mode=="hello_send_failure")fail_send=true;
 if(mode=="create_failure")fail_create=true;
 if(mode=="wire2"||mode=="malformed2")selected_version=2;
 if(mode=="wire3"||mode=="malformed3")selected_version=3;
 if(mode=="waiter_destroy"||mode=="waiter_remote"){
  Barrier waiter;
  before_event_wait=[&](Event* p,unsigned){if(p==created_events.front())waiter.pause();};
  auto protocol=owner.p;
  auto worker=std::async(std::launch::async,[protocol]{return protocol->OpenAudioChannel();});
  waiter.wait();
  if(mode=="waiter_destroy")owner.destroy();else network.transports[0]->Disconnect();
  auto event=created_events.front();
  assert(event->alive);assert((event->bits.load()&2)!=0);
  waiter.release.set_value();assert(!worker.get());before_event_wait={};
  assert(transport_destroys==1);  // 接收线程不清 transport，返回的 hello 等待者应清理。
  assert(closed==1&&opened==0);owner.destroy();owner.unchanged();assert(events_live==0);return 0;
 }
 if(mode=="replace_batch"){
  // 复刻 Application：关闭通知只排队 Idle，Run 整批移出；状态迁移用实际状态机。
  MainTasks main_tasks;DeviceStateMachine states;
  assert(states.TransitionTo(kDeviceStateStarting));
  assert(states.TransitionTo(kDeviceStateActivating));
  assert(states.TransitionTo(kDeviceStateIdle));
  bool performance=false;
  owner.p->OnAudioChannelOpened([&]{++opened;performance=true;});
  owner.p->OnAudioChannelClosed([&]{++closed;performance=false;main_tasks.schedule([&]{states.TransitionTo(kDeviceStateIdle);});});
  auto connect=[&]{
   assert(states.TransitionTo(kDeviceStateConnecting));
   main_tasks.schedule([&]{
    performance=true;  // 复刻 ContinueOpenAudioChannel：连接前先提速
    if(!owner.p->IsAudioChannelOpened()){
     if(!owner.p->OpenAudioChannel()){performance=false;states.TransitionTo(kDeviceStateIdle);return;}
    }
    states.TransitionTo(kDeviceStateListening);
   });
  };
  connect();main_tasks.run();
  assert(opened==1&&states.GetState()==kDeviceStateListening&&performance&&owner.p->IsAudioChannelOpened());
  // 121 秒无入站：仅按超时视为未打开，transport 与状态都尚未退休。
  clock_ms+=121000;assert(!owner.p->IsAudioChannelOpened());
  assert(states.TransitionTo(kDeviceStateIdle));
  bool replace_ok=false;
  assert(states.TransitionTo(kDeviceStateConnecting));
  main_tasks.schedule([&]{
   performance=true;  // 再次按键/唤醒同样先提速
   if(!owner.p->IsAudioChannelOpened()){
    replace_ok=owner.p->OpenAudioChannel();
    if(!replace_ok){performance=false;states.TransitionTo(kDeviceStateIdle);return;}
   }
   states.TransitionTo(kDeviceStateListening);
  });
  main_tasks.run();
  main_tasks.run();  // 旧关闭通知排入的 Idle 批次
  assert(!replace_ok);  // 本次只退休旧通道并交付首次关闭通知
  // 关闭通知已被消费，失败分支必须自行恢复低功耗，否则设备停在性能档。
  assert(states.GetState()==kDeviceStateIdle&&closed==1&&!performance);
  assert(!owner.p->IsAudioChannelOpened());
  connect();main_tasks.run();main_tasks.run();
  assert(opened==2&&states.GetState()==kDeviceStateListening&&performance);
  assert(owner.p->IsAudioChannelOpened());
  owner.p->CloseAudioChannel();main_tasks.run();
  assert(closed==2&&states.GetState()==kDeviceStateIdle);
  owner.destroy();assert(events_live==0&&deleted_event_sets==0);return 0;
 }
 if(mode=="replace_batch_remote_race"){
  // 先排入 ContinueOpen，再在批运行前投递远端断开：旧通道已退休且关闭通知已被
  // 消费，本回合仍不得报告新通道成功，否则排在其后的 Idle 会覆盖 Listening。
  MainTasks main_tasks;DeviceStateMachine states;
  assert(states.TransitionTo(kDeviceStateStarting));
  assert(states.TransitionTo(kDeviceStateActivating));
  assert(states.TransitionTo(kDeviceStateIdle));
  bool performance=false;
  owner.p->OnAudioChannelOpened([&]{++opened;performance=true;});
  owner.p->OnAudioChannelClosed([&]{++closed;performance=false;main_tasks.schedule([&]{states.TransitionTo(kDeviceStateIdle);});});
  auto connect=[&]{
   assert(states.TransitionTo(kDeviceStateConnecting));
   main_tasks.schedule([&]{
    performance=true;  // 复刻 ContinueOpenAudioChannel：连接前先提速
    if(!owner.p->IsAudioChannelOpened()){
     if(!owner.p->OpenAudioChannel()){performance=false;states.TransitionTo(kDeviceStateIdle);return;}
    }
    states.TransitionTo(kDeviceStateListening);
   });
  };
  connect();main_tasks.run();
  assert(opened==1&&states.GetState()==kDeviceStateListening&&performance);
  // 121 秒无入站：业务侧视为未打开并回到 Idle（通道尚未退休）。
  clock_ms+=121000;assert(!owner.p->IsAudioChannelOpened());
  assert(states.TransitionTo(kDeviceStateIdle));
  bool replace_ok=false;
  assert(states.TransitionTo(kDeviceStateConnecting));
  main_tasks.schedule([&]{
   performance=true;  // 排入后远端已关闭，本回合失败仍须恢复低功耗
   if(!owner.p->IsAudioChannelOpened()){
    replace_ok=owner.p->OpenAudioChannel();
    if(!replace_ok){performance=false;states.TransitionTo(kDeviceStateIdle);return;}
   }
   states.TransitionTo(kDeviceStateListening);
  });
  // 批运行前远端断开：退休旧通道并把 Idle 排在 ContinueOpen 之后。
  network.transports[0]->Disconnect();
  assert(closed==1&&!performance);
  main_tasks.run();main_tasks.run();
  assert(!replace_ok);
  assert(states.GetState()==kDeviceStateIdle&&closed==1&&!performance);
  assert(!owner.p->IsAudioChannelOpened());
  connect();main_tasks.run();main_tasks.run();
  assert(opened==2&&states.GetState()==kDeviceStateListening&&performance);
  assert(owner.p->IsAudioChannelOpened());
  owner.p->CloseAudioChannel();main_tasks.run();
  assert(closed==2&&states.GetState()==kDeviceStateIdle);
  owner.destroy();assert(events_live==0&&deleted_event_sets==0);return 0;
 }
 bool ok=owner.p->OpenAudioChannel();
 if(mode=="destructor_window"){
  // 错误态析构：事件组删除前安装钩子，删除后投递迟到 hello，不得再写事件位。
  assert(ok&&opened==1);
  auto stream=network.transports[0]->stream();
  int hook_calls=0;
  after_event_delete=[&](Event* p){
   if(p!=created_events.front())return;
   ++hook_calls;stream(frame(hello_payload));
  };
  fail_send=true;
  auto packet=std::make_unique<AudioStreamPacket>();packet->payload={1};
  assert(!owner.p->SendAudio(std::move(packet)));
  after_event_delete={};
  assert(errors==1&&closed==1&&hook_calls==1);
  assert(!owner.p->IsAudioChannelOpened()&&transport_destroys==1);
  owner.destroy();assert(deleted_event_sets==0);assert(events_live==0);return 0;
 }
 if(mode=="timeout"||mode=="connect_failure"||mode=="hello_send_failure"||mode=="create_failure"){
  assert(!ok&&!owner.p->IsAudioChannelOpened());assert(opened==0&&closed==1);
  assert(transport_destroys==(mode=="create_failure"?0:1));
  owner.p->CloseAudioChannel();owner.destroy();assert(closed==1&&events_live==0);return 0;
 }
 assert(ok&&opened==1&&owner.p->IsAudioChannelOpened());
 assert(owner.p->session_id()=="first"&&owner.p->server_sample_rate()==16000&&owner.p->server_frame_duration()==40);
 auto stream=network.transports[0]->stream();auto lost=network.transports[0]->disconnected();
 if(mode=="active"){
  owner.p->CloseAudioChannel();owner.p->CloseAudioChannel();lost();
  assert(closed==1&&low_power&&idle&&!owner.p->IsAudioChannelOpened());
 }else if(mode=="remote"){
  stream(std::string("\x88\0",2));stream(std::string("\x88\0",2));lost();
  assert(closed==1);assert(transport_destroys==0);owner.p->CloseAudioChannel();assert(closed==1);
 }else if(mode=="replace"){
  // 旧通道首次关闭通知必须单独一轮送达；本次不得报告新通道成功。
  assert(!owner.p->OpenAudioChannel());
  assert(closed==1&&opened==1&&low_power&&idle&&!owner.p->IsAudioChannelOpened());
  assert(transport_destroys==1);
  assert(owner.p->OpenAudioChannel());
  assert(closed==1&&opened==2&&!low_power&&!idle);
  lost();stream(frame(hello_payload));assert(closed==1&&owner.p->IsAudioChannelOpened());
 }else if(mode=="wire1"||mode=="wire2"||mode=="wire3"){
  stream(frame(audio(selected_version),true));assert(received==1);
  auto packet=std::make_unique<AudioStreamPacket>();packet->timestamp=0x12345678;
  packet->payload={0x11,0x22,0x33};assert(owner.p->SendAudio(std::move(packet)));
  assert(unmask(network.transports[0]->sent.back())==audio(selected_version));
  stream(frame(R"({"type":"mcp","payload":{"id":4}})"));assert(jsons==1);
 }else if(mode=="malformed2"||mode=="malformed3"){
  auto bytes=audio(selected_version);
  stream(frame(bytes.substr(0,selected_version==2?15:3),true));
  stream(frame(bytes.substr(0,bytes.size()-1),true));assert(received==0);
  stream(frame(bytes,true));assert(received==1);
 }else if(mode=="bad_hello"){
  for(auto bad:{R"({"type":"hello"})",R"({"type":"hello","transport":4})",R"({"type":"hello","transport":"udp"})"})stream(frame(bad));
  assert(owner.p->session_id()=="first");
 }else if(mode=="timeout_clock"){
  clock_ms+=120000;assert(owner.p->IsAudioChannelOpened());
  clock_ms+=1000;assert(!owner.p->IsAudioChannelOpened());
  stream(frame(R"({"type":"mcp"})"));assert(owner.p->IsAudioChannelOpened());
 }else if(mode=="text_failure"||mode=="audio_failure"){
  fail_send=true;
  if(mode=="text_failure")owner.p->SendMcpMessage("{}");
  else {auto packet=std::make_unique<AudioStreamPacket>();packet->payload={1};assert(!owner.p->SendAudio(std::move(packet)));}
  assert(errors==1&&closed==1&&!owner.p->IsAudioChannelOpened());assert(transport_destroys==1);
 }else if(mode=="hello_destroy"||mode=="hello_replace"||mode=="json_parse_destroy"){
  Barrier parsing;
  auto incoming=mode=="json_parse_destroy"?std::string(R"({"type":"mcp"})"):
   std::string(R"({"type":"hello","transport":"websocket","session_id":"old","audio_params":{"sample_rate":8000,"frame_duration":20}})");
  auto worker=std::async(std::launch::async,[&]{parse_barrier=&parsing;stream(frame(incoming));});
  parsing.wait();
  if(mode=="hello_replace"){
   hello_payload=R"({"type":"hello","transport":"websocket","session_id":"new","audio_params":{"sample_rate":48000,"frame_duration":60}})";
   // 旧通道先由远端退休并已交付关闭通知；第一次打开只消费旧通道并返回失败，
   // 下一次显式打开才建立新会话，旧解析不得污染新状态。
   network.transports[0]->Disconnect();
   assert(closed==1);
   assert(!owner.p->OpenAudioChannel());
   assert(owner.p->OpenAudioChannel());
  }else owner.destroy();
  clock_ms+=1000;parsing.release.set_value();worker.get();
  if(mode=="hello_replace")assert(owner.p->session_id()=="new"&&owner.p->server_sample_rate()==48000&&owner.p->server_frame_duration()==60);
  else {owner.unchanged();assert(events_live==0&&deleted_event_sets==0);}
 }else if(mode=="audio_block"||mode=="json_block"||mode=="audio_reenter"||mode=="json_reenter"){
  // 回调副本须在连接创建时登记；重新打开后使用实际传输入口。
  Barrier downstream;bool reenter=mode.find("reenter")!=std::string::npos;
  if(mode.find("audio")==0)owner.p->OnIncomingAudio([&](std::unique_ptr<AudioStreamPacket> p){
   assert(p->payload==std::vector<uint8_t>({0x11,0x22,0x33}));if(reenter)owner.destroy();else downstream.pause();clock_ms+=1000;
  });else owner.p->OnIncomingJson([&](const cJSON* root){
   if(reenter)owner.destroy();else downstream.pause();clock_ms+=1000;assert(cJSON_IsString(cJSON_GetObjectItem(root,"type")));
  });
  owner.p->CloseAudioChannel();
  assert(owner.p->OpenAudioChannel());stream=network.transports.back()->stream();
  auto worker=std::async(std::launch::async,[&]{stream(mode.find("audio")==0?frame(audio(1),true):frame(R"({"type":"mcp"})"));});
  if(!reenter){downstream.wait();owner.destroy();downstream.release.set_value();}
  assert(worker.wait_for(2s)==std::future_status::ready);worker.get();owner.unchanged();assert(events_live==0);
 }else if(mode=="close_reenter"||mode=="replace_reenter"||mode=="error_reenter"||mode=="opened_reenter"){
  owner.p->CloseAudioChannel();
  owner.p->OnAudioChannelClosed([&]{++closed;owner.destroy();});
  if(mode=="error_reenter")owner.p->OnNetworkError([&](const std::string&){++errors;owner.destroy();});
  if(mode=="opened_reenter")owner.p->OnAudioChannelOpened([&]{++opened;owner.destroy();});
  auto protocol=owner.p;bool opened_ok=protocol->OpenAudioChannel();
  // opened 回调销毁协议后必须报告失败，不得让已退休通道的成功覆盖 Idle。
  assert(opened_ok==(mode!="opened_reenter"));
  if(mode=="close_reenter")protocol->CloseAudioChannel();
  if(mode=="replace_reenter")assert(!protocol->OpenAudioChannel());
  if(mode=="error_reenter"){fail_send=true;protocol->SendMcpMessage("{}");}
  assert(owner.p==nullptr);owner.unchanged();assert(events_live==0);
 }else if(mode=="close_race"){
  owner.p->CloseAudioChannel();Barrier notifying;
  owner.p->OnAudioChannelClosed([&]{++closed;notifying.pause();});assert(owner.p->OpenAudioChannel());
  stream=network.transports.back()->stream();auto worker=std::async(std::launch::async,[stream]{stream(std::string("\x88\0",2));});
  notifying.wait();owner.p->CloseAudioChannel();notifying.release.set_value();worker.get();assert(closed==2);
 }else assert(false);
 owner.destroy();assert(events_live==0&&deleted_event_sets==0);return 0;
}
'''


class WebsocketProtocolLifetimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('CXX') or shutil.which('g++')
        if not compiler or not COMPONENT.exists() or not CJSON.exists():
            raise AssertionError('需要 host C++ 编译器、已应用补丁的 esp-ml307 及 cJSON')
        cls.temp = tempfile.TemporaryDirectory(prefix='s3-websocket-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        files = dict(STUBS, **{
            'freertos/FreeRTOS.h': FREERTOS,
            'freertos/event_groups.h': '#include "FreeRTOS.h"\n',
            'network_interface.h': NETWORK,
            'wire.h': WIRE,
            'clock.h': CLOCK,
            'esp_log.h': '\n'.join('#define ESP_LOG'+level+'(...) do{}while(0)' for level in 'EWIDV'),
            'esp_pthread.h': '',
            'probe.cpp': PROGRAM,
        })
        for relative, content in files.items():
            target = folder / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding='utf-8')
        cls.exe = folder / ('probe.exe' if os.name == 'nt' else 'probe')
        cls.env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get('PATH', ''))
        commands = [
            [compiler, '-x', 'c', '-std=c11', '-O0', '-I'+str(CJSON), '-c', str(CJSON/'cJSON.c'), '-o', str(folder/'cJSON.o')],
            [compiler, '-std=c++17', '-O0', '-pthread', '-include', str(folder/'clock.h'),
             '-I'+str(folder), '-I'+str(COMPONENT/'include'), '-I'+str(ROOT/'main'),
             '-I'+str(ROOT/'main/protocols'), '-I'+str(CJSON),
             str(folder/'probe.cpp'), str(COMPONENT/'src/web_socket.cc'),
             str(ROOT/'main/protocols/websocket_protocol.cc'), str(ROOT/'main/protocols/protocol.cc'),
             str(ROOT/'main/device_state_machine.cc'),
             str(folder/'cJSON.o'), '-o', str(cls.exe)],
        ]
        for command in commands:
            built = subprocess.run(command, env=cls.env, capture_output=True, timeout=60)
            if built.returncode:
                raise AssertionError(built.stdout.decode('utf-8', 'replace') + built.stderr.decode('utf-8', 'replace'))

    def check(self, mode):
        ran = subprocess.run([str(self.exe), mode], env=self.env, capture_output=True, timeout=10)
        self.assertEqual(ran.returncode, 0, mode+'\n'+ran.stdout.decode('utf-8', 'replace')+ran.stderr.decode('utf-8', 'replace'))

    def test_active_close_restores_business_state_once(self): self.check('active')
    def test_remote_close_notifies_once_without_destroying_transport(self): self.check('remote')
    def test_remote_and_active_close_race(self): self.check('close_race')
    def test_replacement_retires_old_channel(self): self.check('replace')
    def test_replacement_waits_for_queued_application_idle_batch(self): self.check('replace_batch')
    def test_remote_close_before_open_does_not_clobber_new_session(self): self.check('replace_batch_remote_race')
    def test_hello_timeout_releases_transport(self): self.check('timeout')
    def test_error_state_destruction_has_no_deleted_event_access(self): self.check('destructor_window')
    def test_connect_failure_retires_channel(self): self.check('connect_failure')
    def test_create_failure_retires_channel(self): self.check('create_failure')
    def test_hello_send_failure_retires_channel(self): self.check('hello_send_failure')
    def test_text_send_failure_retires_channel(self): self.check('text_failure')
    def test_audio_send_failure_retires_channel(self): self.check('audio_failure')
    def test_hello_waiter_survives_destruction_and_is_woken(self): self.check('waiter_destroy')
    def test_remote_close_wakes_hello_waiter(self): self.check('waiter_remote')
    def test_entered_hello_survives_owner_destruction(self): self.check('hello_destroy')
    def test_entered_old_hello_cannot_pollute_new_channel(self): self.check('hello_replace')
    def test_entered_json_parse_survives_owner_destruction(self): self.check('json_parse_destroy')
    def test_blocked_audio_does_not_hold_lifetime_lock(self): self.check('audio_block')
    def test_blocked_json_does_not_hold_lifetime_lock(self): self.check('json_block')
    def test_audio_callback_can_destroy_owner(self): self.check('audio_reenter')
    def test_json_callback_can_destroy_owner(self): self.check('json_reenter')
    def test_close_notification_can_destroy_owner(self): self.check('close_reenter')
    def test_replace_notification_can_destroy_owner(self): self.check('replace_reenter')
    def test_error_notification_can_destroy_owner(self): self.check('error_reenter')
    def test_opened_notification_can_destroy_owner(self): self.check('opened_reenter')
    def test_binary_v1_and_json_wire_behavior(self): self.check('wire1')
    def test_binary_v2_and_json_wire_behavior(self): self.check('wire2')
    def test_binary_v3_and_json_wire_behavior(self): self.check('wire3')
    def test_v2_rejects_truncated_header_and_payload(self): self.check('malformed2')
    def test_v3_rejects_truncated_header_and_payload(self): self.check('malformed3')
    def test_malformed_hello_does_not_change_negotiation(self): self.check('bad_hello')
    def test_receive_timestamp_preserves_120_second_timeout(self): self.check('timeout_clock')

    def test_application_close_callback_defers_idle_to_next_batch(self):
        # 关闭通知只排队 Idle：替换路径若同步报告成功，排队 Idle 会覆盖新会话。
        source = (ROOT / 'main/application.cc').read_text(encoding='utf-8')
        close = source[source.index('protocol_->OnAudioChannelClosed('):]
        close = close[:close.index('\n    });')]
        self.assertIn('SetPowerSaveLevel(PowerSaveLevel::LOW_POWER)', close)
        self.assertIn('Schedule([this]()', close)
        self.assertIn('SetDeviceState(kDeviceStateIdle)', close)

    def test_application_open_failure_restores_low_power(self):
        # 旧关闭通知可能已被上一次 OpenAudioChannel 消费；调用方拿到 false 后
        # 不能依赖不会再来的关闭回调，必须自行恢复低功耗。
        source = (ROOT / 'main/application.cc').read_text(encoding='utf-8')
        for name in ('ContinueOpenAudioChannel', 'ContinueWakeWordInvoke'):
            body = source[source.index('void Application::' + name + '('):]
            body = body[:body.index('\n}\n')]
            failed = body[body.index('if (!protocol_->OpenAudioChannel()) {'):]
            failed = failed[:failed.index('\n        }')]
            self.assertIn('SetPowerSaveLevel(PowerSaveLevel::LOW_POWER)', failed, name)
            self.assertIn('SetDeviceState(kDeviceStateIdle)', failed, name)

    def test_application_run_moves_task_batch_before_execution(self):
        # 与探针 MainTasks 同一语义：执行期间新排队任务留到下一批。
        source = (ROOT / 'main/application.cc').read_text(encoding='utf-8')
        run = source[source.index('if (bits & MAIN_EVENT_SCHEDULE)'):]
        run = run[:run.index('if (bits & MAIN_EVENT_CLOCK_TICK)')]
        self.assertIn('auto tasks = std::move(main_tasks_);', run)
        self.assertIn('for (auto& task : tasks)', run)
        schedule = source[source.index('void Application::Schedule('):]
        schedule = schedule[:schedule.index('\n}') + 2]
        self.assertIn('main_tasks_.push_back(std::move(callback))', schedule)
        self.assertIn('MAIN_EVENT_SCHEDULE', schedule)


if __name__ == '__main__':
    unittest.main()
