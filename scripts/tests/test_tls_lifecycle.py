"""编译真实Disconnect/ReceiveTask，模拟描述符回收并复现旧TLS关闭新连接。"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'managed_components/78__esp-ml307/src/esp/esp_ssl.cc'
def function(source,signature):
    start=source.index(signature);brace=source.index('{',start);depth=1;i=brace+1
    while depth:
        depth+=(source[i]=='{')-(source[i]=='}');i+=1
    return source[start:i]
PROGRAM=r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <string>
#include <functional>
#include <map>
struct esp_tls_t {int sockfd=3;};
int mode=0,destroyed=0,closed=0,shutdowns=0,callback_count=0,foreign_fd=-1;
std::map<int,int> owners;
bool saw_tls_before_worker_exit=false;
using EventBits_t=int;
constexpr int ESP_OK=0,ESP_SSL_EVENT_RECEIVE_TASK_EXIT=1,ESP_TLS_ERR_SSL_WANT_READ=-2;
constexpr int SHUT_RDWR=2,MALLOC_CAP_INTERNAL=1;
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
#define pdFALSE 0
#define pdMS_TO_TICKS(x) x
#define ESP_LOGE(...) do{}while(0)
#define ESP_LOGW(...) do{}while(0)
#define ESP_LOGI(...) do{}while(0)
const char* TAG="tls";
int close(int fd){++closed;owners.erase(fd);return 0;}
int shutdown(int fd,int){assert(owners[fd]==1);++shutdowns;return 0;}
int allocate_foreign(){int fd=3;while(owners.count(fd))++fd;owners[fd]=2;return fd;}
int esp_tls_get_conn_sockfd(esp_tls_t* p,int* out){*out=p->sockfd;return ESP_OK;}
int esp_tls_conn_destroy(esp_tls_t* p){++destroyed;if(p->sockfd>=0)close(p->sockfd);delete p;return 0;}
int xEventGroupWaitBits(int,int,int,int,int){foreign_fd=allocate_foreign();saw_tls_before_worker_exit=destroyed==0;return 1;}
size_t heap_caps_get_free_size(int){return 32000;}
size_t heap_caps_get_largest_free_block(int){return 16000;}
int esp_tls_conn_read(esp_tls_t*,char*,size_t){errno=EIO;return -76;}
struct EspSsl {
 bool connected_=true;esp_tls_t* tls_client_=nullptr;int event_group_=0,last_error_=0;
 std::function<void()> disconnect_callback_;
 std::function<void(const std::string&)> stream_callback_;
 void Disconnect();void ReceiveTask();
};
@@FUNCTIONS@@
int main(int argc,char**argv){
 mode=atoi(argv[1]);EspSsl ssl;owners[3]=1;
 if(mode!=3)ssl.tls_client_=new esp_tls_t();
 if(mode==2){ssl.disconnect_callback_=[](){++callback_count;};ssl.ReceiveTask();assert(!ssl.connected_);assert(callback_count==1);assert(ssl.last_error_==-76);}
 ssl.Disconnect();ssl.Disconnect();
 if(mode!=3){assert(saw_tls_before_worker_exit);assert(destroyed==1);assert(closed==1);assert(owners[foreign_fd]==2);assert(owners.size()==1);}
 else assert(destroyed==0 && closed==0 && shutdowns==0);
 assert(!ssl.connected_);printf("PASS mode=%d closes=%d shutdowns=%d foreign=%d\n",mode,closed,shutdowns,foreign_fd);
}
'''
class TlsLifecycleTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
    source=Path(os.environ.get('TLS_SOURCE',SOURCE)).read_text(encoding='utf-8')
    cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
    file=Path(cls.temp.name)/'probe.cpp';cls.exe=file.with_suffix('.exe' if os.name=='nt' else '')
    file.write_text(PROGRAM.replace('@@FUNCTIONS@@',function(source,'void EspSsl::Disconnect()')+'\n'+function(source,'void EspSsl::ReceiveTask()')),encoding='utf-8')
    compiler=os.environ.get('CXX') or shutil.which('g++');assert compiler
    cls.env=dict(os.environ);cls.env['PATH']=str(Path(compiler).parent)+os.pathsep+cls.env.get('PATH','')
    result=subprocess.run([compiler,'-std=c++17','-O0',str(file),'-o',str(cls.exe)],capture_output=True,env=cls.env,timeout=60)
    if result.returncode:raise AssertionError(result.stderr.decode('utf-8','replace'))
 def check(self,mode):
    result=subprocess.run([str(self.exe),str(mode)],capture_output=True,env=self.env,timeout=5)
    self.assertEqual(result.returncode,0,result.stdout.decode()+result.stderr.decode())
 def test_shutdown_does_not_recycle_fd_before_tls_destroy(self):self.check(0)
 def test_disconnected_transport_still_has_one_socket_owner(self):self.check(1)
 def test_unexpected_read_failure_records_error_and_notifies_once(self):self.check(2)
 def test_uninitialized_disconnect_is_idempotent(self):self.check(3)
if __name__=='__main__':unittest.main()
