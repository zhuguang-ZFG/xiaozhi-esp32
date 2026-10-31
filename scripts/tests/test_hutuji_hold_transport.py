"""真实发送循环：每次socket写前核会话，并保留普通行背压期间的暂停抢占。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_hutuji_ota_reservation import function

ROOT = Path(__file__).resolve().parents[2]
PROGRAM = r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include <future>
#include <chrono>
#include "main/boards/lichuang-dev/hutuji_recovery_core.h"
using namespace hutuji;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
constexpr unsigned portTICK_PERIOD_MS=1;
constexpr int MSG_DONTWAIT=0,SHUT_RDWR=0;
unsigned ticks=0;int attempts=0,mode=0,shutdowns=0;
std::vector<int> sockets;
std::function<void()> at_lock,at_delay;
unsigned xTaskGetTickCount(){return ticks;}
void vTaskDelay(unsigned n){ticks+=n;if(at_delay){auto f=std::move(at_delay);at_delay={};f();}}
struct SocketMutex{std::mutex m;void lock(){if(at_lock){auto f=std::move(at_lock);at_lock={};f();}m.lock();}void unlock(){m.unlock();}};
int send(int socket,const char*,size_t length,int){++attempts;
 if(mode==4 || mode==5 || (mode==3 && attempts==1)){errno=EAGAIN;return -1;}
 sockets.push_back(socket);return int(length);
}
int shutdown(int,int){++shutdowns;return 0;}
struct Pipe{
 std::mutex write_mutex_;SocketMutex sock_mutex_;
 std::atomic<bool> connected_{true},ready_{true},authorized_{true},settings_verified_{true},task_session_active_{true};
 std::atomic<unsigned> connection_seq_{7},reset_banner_seq_{9},feed_hold_waiters_{0};
 int sock_=70;
 struct Token{void Cancel(){}}abort_reset_token_;
 bool IsCommandSessionCurrent(uint32_t,uint32_t)const;
 bool SendRawLockedForSession(const char*,size_t,bool,uint32_t,uint32_t);
 bool SendFeedHoldForSession(uint32_t,uint32_t);
 void ShutdownSocket(uint32_t);
};
@@METHODS@@
int main(int argc,char**argv){mode=std::stoi(argv[1]);Pipe p;
 if(mode==1)++p.connection_seq_;
 if(mode==2)at_lock=[&]{++p.reset_banner_seq_;};
 if(mode==3)at_delay=[&]{++p.connection_seq_;p.sock_=80;};
 if(mode==5){++p.connection_seq_;p.sock_=80;}
 bool ok;
 if(mode==6){p.write_mutex_.lock();auto task=std::async(std::launch::async,[&]{return p.SendFeedHoldForSession(7,9);});
 assert(task.wait_for(std::chrono::milliseconds(250))==std::future_status::ready);
 ok=task.get();p.write_mutex_.unlock();}
 else ok=p.SendFeedHoldForSession(7,9);
 assert(ok==(mode==0 || mode==6));assert(p.feed_hold_waiters_==0);
 assert(sockets.size()==(ok?1u:0u));if(ok)assert(sockets[0]==70);
 assert(shutdowns==(mode==4?1:0));
 if(mode==4)assert(!p.ready_ && !p.authorized_);
}
'''


class HoldTransportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = (ROOT / 'main/boards/lichuang-dev/hutuji_pipe.cc').read_text(encoding='utf-8')
        methods = '\n'.join(function(source, name) for name in (
            'bool Pipe::IsCommandSessionCurrent(', 'bool Pipe::SendRawLockedForSession(',
            'bool Pipe::SendFeedHoldForSession(', 'void Pipe::ShutdownSocket('))
        methods = methods.replace('std::lock_guard<std::mutex> lock(sock_mutex_)',
                                  'std::lock_guard<SocketMutex> lock(sock_mutex_)')
        methods = methods.replace('std::lock_guard<std::mutex> sock_lock(sock_mutex_)',
                                  'std::lock_guard<SocketMutex> sock_lock(sock_mutex_)')
        cpp = Path(cls.temp.name) / 'hold.cpp'
        cls.exe = cpp.with_suffix('.exe')
        cpp.write_text(PROGRAM.replace('@@METHODS@@', methods), encoding='utf-8')
        build = subprocess.run([os.environ['CXX'], '-std=c++17', '-pthread', '-I', str(ROOT),
                                str(cpp), '-o', str(cls.exe)], capture_output=True, timeout=60)
        assert build.returncode == 0, build.stderr.decode('utf-8', 'replace')

    def test_transport_interleavings(self):
        for mode in range(7):
            with self.subTest(mode=mode):
                result = subprocess.run([str(self.exe), str(mode)], capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', 'replace'))
