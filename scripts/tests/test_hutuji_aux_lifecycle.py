"""真实激活任务退出与会话截断回归；平台调用使用可观察替身。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get("XIAOZHI_ROOT", Path(__file__).resolve().parents[2]))

def body(source, signature):
    start = source.index(signature)
    left = source.index("{", start)
    depth = 1
    end = left + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

class AuxiliaryLifecycleTest(unittest.TestCase):
    def run_cpp(self, source):
        compiler = os.environ.get("CXX") or shutil.which("g++")
        if not compiler:
            self.skipTest("需要宿主 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "probe.cpp"
            path.write_text(source, encoding="utf-8")
            exe = path.with_suffix(".exe" if os.name == "nt" else "")
            env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
            compiled = subprocess.run([compiler, "-std=c++17", str(path), "-o", str(exe)], env=env, capture_output=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr.decode("utf-8", "replace"))
            result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
            return result.stdout

    def test_relay_destroys_http_before_task_deletion_on_every_exit(self):
        raw = (ROOT / "main/boards/lichuang-dev/hutuji_activation_relay.cc").read_text(encoding="utf-8")
        source = "\n".join(line for line in raw.splitlines() if not line.startswith("#include"))
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <csetjmp>
#include <memory>
#include <string>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
int scenario=0, live=0, made=0, deleted=0;
std::jmp_buf escaped;
void vTaskDelete(void*) { ++deleted; std::longjmp(escaped,1); }
constexpr int pdPASS=1;
int xTaskCreate(void(*)(void*),const char*,uint32_t,void*,int,void*) {return 0;}
struct cJSON {};
cJSON* cJSON_CreateObject(){return new cJSON;}
void cJSON_AddStringToObject(cJSON*,const char*,const char*){}
void cJSON_Delete(cJSON* p){delete p;}
char* cJSON_PrintUnformatted(cJSON*){if(scenario==3)return nullptr;char* p=(char*)std::malloc(3);std::memcpy(p,"{}",3);return p;}
void cJSON_free(void* p){std::free(p);}
struct Http {
    Http(){++live;++made;} ~Http(){--live;}
    void SetHeader(const char*,const char*){} void SetContent(std::string){}
    bool Open(const char*,const char*){return scenario!=1;} int GetStatusCode(){return 200;} void Close(){}
};
struct Network {std::unique_ptr<Http> CreateHttp(int){return scenario==4?nullptr:std::make_unique<Http>();}};
struct Board {
    static Board& GetInstance(){static Board value;return value;}
    Network* GetNetwork(){static Network n;return scenario==2?nullptr:&n;}
};
struct SystemInfo {static std::string GetMacAddress(){return "00:00:00:00:00:00";}};
''' + source + r'''
int main(){
    for(scenario=0;scenario<5;++scenario){
        if(setjmp(escaped)==0) hutuji::RelayTask(new hutuji::RelayPayload{});
        assert(live==0);
    }
    assert(made==2 && deleted==5);
}
''')

    def test_conversation_truncates_only_at_complete_utf8_boundary(self):
        raw = (ROOT / "main/boards/lichuang-dev/hutuji_conversation_report.cc").read_text(encoding="utf-8")
        output = self.run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
constexpr size_t kMaxContent=500,kMaxSession=64,kQueueCap=8;
constexpr int pdTRUE=1;
#define pdMS_TO_TICKS(x) (x)
int xSemaphoreTake(void*,int){return pdTRUE;} void xSemaphoreGive(void*){}
int64_t esp_timer_get_time(){return 0;}
void* g_mutex=(void*)1;
size_t g_count=0,g_head=0;
struct PendingTurn{char role[16],session_id[kMaxSession],content[kMaxContent+1];double client_ts;};
PendingTurn g_queue[kQueueCap];
''' + body(raw, "bool PushTurn(") + r'''
int main(){
    for(const std::string& ch:{std::string(u8"中"),std::string(u8"😀"),std::string("A")}){
        for(int offset=0;offset<5;++offset){
            std::string input(offset,'a'); for(int j=0;j<501;++j)input+=ch;
            g_count=0;g_head=0; assert(PushTurn("user",input.c_str(),"session"));
            assert(std::strlen(g_queue[0].content)<=500);
            std::cout<<g_queue[0].content<<"\n";
        }
    }
    assert(PushTurn("user",u8"短中文😀","session"));
    assert(std::string(g_queue[1].content)==u8"短中文😀");
}
''')
        rows = output.decode("utf-8").splitlines()
        self.assertEqual(len(rows), 15)
        for index, row in enumerate(rows):
            char = ["中", "😀", "A"][index // 5]
            offset = index % 5
            self.assertEqual(row, "a" * offset + char * ((500 - offset) // len(char.encode())))

if __name__ == "__main__":
    unittest.main()
