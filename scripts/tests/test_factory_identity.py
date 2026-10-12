"""真实UTF-8校验/工厂身份宿主回归。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
CXX=os.environ.get('CXX','D:/zhugu-home/mingw64/mingw64/bin/g++.exe')
class FactoryTest(unittest.TestCase):
    def compile_run(self,source,headers=None):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary);cpp=path/'test.cpp';cpp.write_text(source,encoding='utf-8')
            for name,text in (headers or {}).items():(path/name).write_text(text,encoding='utf-8')
            exe=path/'test.exe'
            build=subprocess.run([CXX,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(path),'-I',str(ROOT),str(cpp),'-o',str(exe)],capture_output=True,timeout=60)
            self.assertEqual(build.returncode,0,build.stderr.decode('utf-8','replace'))
            env=dict(os.environ,PATH=str(Path(CXX).parent)+os.pathsep+os.environ.get('PATH',''))
            run=subprocess.run([str(exe)],capture_output=True,env=env,timeout=15)
            self.assertEqual(run.returncode,0,run.stderr.decode('utf-8','replace'))

    def test_utf8_matrix(self):
        self.compile_run('\n#include <cassert>\n#include <string>\n#include "main/boards/lichuang-dev/utf8_ssid.h"\nint main(){\n    auto check=[](const std::string& s){return ValidUtf8Ssid(s.data(),s.size());};\n    assert(check("Home Wifi"));assert(check("家里网络"));assert(check("家庭😀"));\n    assert(check(std::string(32,\'a\')));assert(!check(std::string(33,\'a\')));\n    assert(!check(""));assert(!check("a\\nb"));assert(!check(std::string("a\\0b",3)));\n    assert(!check("\\xc0\\xaf"));assert(!check("\\xe5\\xae"));assert(!check("\\xed\\xa0\\x80"));\n    assert(!check("\\xf4\\x90\\x80\\x80"));assert(!check("\\xc2\\x85"));assert(!check("\\xe2\\x80\\xae"));\n}\n')

    def test_factory_nvs_and_peer_matrix(self):
        self.compile_run('\n#include <cassert>\n#include "main/boards/lichuang-dev/factory_identity.h"\nint main(){\n    using namespace hutuji;\n    auto value=LoadFactoryIdentity();assert(value.present&&value.valid);\n    assert(FactoryPeerMatches(value,"[MSG:Mode=STA:SSID=home:Status=Connected:IP=192.168.1.8:MAC=02-00-00-00-00-02]\\r\\n"));\n    assert(!FactoryPeerMatches(value,"[MSG:Mode=STA:SSID=MAC=02-00-00-00-00-02:Status=Connected:IP=192.168.1.8:MAC=02-00-00-00-00-03]\\r\\n"));\n    assert(!FactoryPeerMatches(value,"[MSG:Mode=STA:SSID=home:Status=Connected:IP=192.168.1.8:MAC=02-00-00-00-00-020]\\r\\n"));\n    for(int i=2;i<=5;++i){scenario=i;value=LoadFactoryIdentity();assert(value.present&&!value.valid);assert(!FactoryPeerMatches(value,""));}\n    scenario=1;value=LoadFactoryIdentity();assert(!value.present&&FactoryPeerMatches(value,""));\n}\n',{'nvs.h': '\n#pragma once\n#include <cstdint>\n#include <cstring>\n#include <string>\nusing esp_err_t=int;using nvs_handle_t=int;\nconstexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,ESP_MAC_WIFI_STA=0;\ninline int scenario=0;\ninline int nvs_open(const char*,int,int* h){*h=1;return scenario==1?ESP_ERR_NVS_NOT_FOUND:scenario==2?2:0;}\ninline int nvs_get_u8(int,const char*,uint8_t* v){*v=scenario==3?2:1;return 0;}\ninline int nvs_get_str(int,const char* key,char* out,size_t*){\n    std::string k=key,v=k=="sn"?"SN001":k=="s3"?"02:00:00:00:00:01":k=="grbl"?"02:00:00:00:00:02":"02:00:00:00:00:03";\n    if(scenario==4&&k=="grbl")return 2;\n    if(scenario==5&&k=="s3")v="02:00:00:00:00:04";\n    std::strcpy(out,v.c_str());return 0;\n}\ninline void nvs_close(int){}\ninline int esp_read_mac(uint8_t* out,int){uint8_t v[6]={2,0,0,0,0,1};std::memcpy(out,v,6);return 0;}\n', 'esp_mac.h': '#include "nvs.h"'})

if __name__=='__main__':unittest.main()
