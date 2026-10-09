"""编译当前真实函数，注入断流/时间交错；不连接或驱动设备。"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from test_hutuji_abort_home_ack import function

ROOT = Path(__file__).resolve().parents[2]
PROGRAM = r'''
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using esp_ota_handle_t = int;
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_OTA_VALIDATE_FAILED=1, OTA_WITH_SEQUENTIAL_WRITES=0, MALLOC_CAP_INTERNAL=0;
struct esp_image_header_t { char bytes[24]; };
struct esp_image_segment_header_t { char bytes[8]; };
struct esp_app_desc_t { char bytes[256]; };
struct Partition { const char* label="ota_1"; unsigned long address=0x20000; };
int scenario=0, begins=0, aborts=0, ends=0, writes=0, active=0, buffers=0;
Partition part;
Partition* esp_ota_get_next_update_partition(void*) {return &part;}
int esp_ota_begin(Partition*, int, int* h) { if(scenario==6)return 2; *h=++begins; ++active; return scenario==7 ? 2 : 0; }
int esp_ota_abort(int h) { if(h) {++aborts; --active;} return 0; }
int esp_ota_write(int, const char*, size_t) { ++writes; return scenario==3 ? 2 : 0; }
int esp_ota_end(int) { assert(buffers==0); ++ends; --active; return scenario==4 ? 2 : 0; }
int esp_ota_set_boot_partition(Partition*) { return scenario==5 ? 2 : 0; }
void* heap_caps_malloc(size_t n,int) { ++buffers; return malloc(n); }
void heap_caps_free(void* p) { --buffers; free(p); }
long long esp_timer_get_time() { static long long t=0; return t+=1000000; }
struct Http {
 int n=0;
 bool Open(const char*,const std::string&) { return true; }
 int GetStatusCode() { return 200; }
 size_t GetBodyLength() { return 8192; }
 int Read(char* p,size_t len) {
  ++n;
  if (scenario==8 && n==2) throw std::runtime_error("read");
  if (((scenario==1 || scenario==10) && n==2) || scenario==2) return -1;
  if (n>2) return 0;
  memset(p,0,len); return (int)len;
 }
 void Close() {}
};
struct Network { std::unique_ptr<Http> CreateHttp(int) { return std::make_unique<Http>(); } } network;
struct Board { static Board& GetInstance(){ static Board b; return b; } Network* GetNetwork(){return &network;} };
struct Ota { static bool Upgrade(const std::string&, std::function<void(int,size_t)>); };
@@METHOD@@

int main(int argc,char**argv) {
 scenario=atoi(argv[1]); bool ok=false; int callbacks=0;
 for(int i=0;i<(scenario==10 ? 3 : 1);++i) {
  try { ok=Ota::Upgrade("https://example.invalid/firmware.bin", [&](int,size_t){if(scenario==9 && ++callbacks==2)throw std::runtime_error("callback");}); }
  catch(const std::exception&) { ok=false; }
 }
 printf("{\"success\":%s,\"begins\":%d,\"aborts\":%d,\"ends\":%d,\"active_handles\":%d,\"buffers\":%d}\n",ok?"true":"false",begins,aborts,ends,active,buffers);
}
'''
CASES = {'test_success': (0, {'success': True, 'aborts': 0, 'ends': 1, 'active_handles': 0, 'buffers': 0}), 'test_read_failure_after_write': (1, {'success': False, 'aborts': 1, 'ends': 0, 'active_handles': 0, 'buffers': 0}), 'test_read_failure_before_begin': (2, {'success': False, 'aborts': 0, 'ends': 0, 'active_handles': 0, 'buffers': 0}), 'test_write_failure': (3, {'success': False, 'aborts': 1, 'ends': 0, 'active_handles': 0, 'buffers': 0}), 'test_end_failure_no_double_abort': (4, {'success': False, 'aborts': 0, 'ends': 1, 'active_handles': 0, 'buffers': 0}), 'test_boot_failure_after_end': (5, {'success': False, 'aborts': 0, 'ends': 1, 'active_handles': 0, 'buffers': 0}), 'test_begin_failure_without_handle': (6, {'success': False, 'aborts': 0, 'ends': 0, 'active_handles': 0, 'buffers': 0}), 'test_begin_failure_with_handle': (7, {'success': False, 'aborts': 1, 'ends': 0, 'active_handles': 0, 'buffers': 0}), 'test_read_exception': (8, {'success': False, 'aborts': 1, 'ends': 0, 'active_handles': 0, 'buffers': 0}), 'test_callback_exception': (9, {'success': False, 'aborts': 1, 'ends': 0, 'active_handles': 0, 'buffers': 0}), 'test_repeated_read_failure': (10, {'success': False, 'aborts': 3, 'ends': 0, 'active_handles': 0, 'buffers': 0})}

class RegressionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("CXX") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("需要host C++编译器")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = Path(cls.temp.name) / "probe.cpp"
        cls.exe = cpp.with_suffix(".exe" if os.name == "nt" else "")
        source = (ROOT / "main/ota.cc").read_text(encoding="utf-8")
        program = PROGRAM.replace("@@METHOD@@", function(source, "bool Ota::Upgrade("))
        cpp.write_text(program, encoding="utf-8")
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.env.get("PATH", "")
        built = subprocess.run([compiler, "-std=c++17", "-O0", str(cpp), "-o", str(cls.exe)],
                               capture_output=True, env=cls.env, timeout=60)
        if built.returncode:
            raise AssertionError(built.stderr.decode("utf-8", "replace"))

def case_test(case, expected):
    def run(self):
        proc = subprocess.run([str(self.exe), str(case)], capture_output=True,
                              env=self.env, timeout=10, check=True)
        result = json.loads(proc.stdout)
        for key, value in expected.items():
            self.assertEqual(result[key], value, (key, result))
    return run

for name, (case, expected) in CASES.items():
    setattr(RegressionTest, name, case_test(case, expected))

if __name__ == "__main__":
    unittest.main()
