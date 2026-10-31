"""OTA下载异常后恢复音频和设备状态，仍由工具层释放任务占位。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_hutuji_abort_home_ack import function

ROOT = Path(__file__).resolve().parents[2]

PROGRAM = r'''
#include <string>
#include <functional>
#include <stdexcept>
#include <new>
#include <iostream>
#include <cstdio>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(int){}
enum DeviceState{kDeviceStateIdle,kDeviceStateUpgrading};
enum class PowerSaveLevel{PERFORMANCE,LOW_POWER};
namespace Lang{namespace Strings{const char *OTA_UPGRADE="upgrade",*UPGRADING="upgrading",*NEW_VERSION="new",*ERROR="error",*UPGRADE_FAILED="failed";}namespace Sounds{const char *OGG_UPGRADE="",*OGG_EXCLAMATION="";}}
struct Display{void SetChatMessage(const char*,const char*){}};
struct Board{Display display;PowerSaveLevel power=PowerSaveLevel::LOW_POWER;
 static Board& GetInstance(){static Board b;return b;}Display* GetDisplay(){return &display;}void SetPowerSaveLevel(PowerSaveLevel v){power=v;}};
struct Protocol{bool IsAudioChannelOpened(){return false;}void CloseAudioChannel(){}};
bool throw_failure=false;
struct Ota{static bool Upgrade(std::string,std::function<void(int,size_t)>,std::string,std::string){if(throw_failure)throw std::bad_alloc();return false;}};
struct Audio{bool running=true;void Stop(){running=false;}void Start(){running=true;}};
struct Application{Audio audio_service_;Protocol* protocol_=nullptr;DeviceState state=kDeviceStateIdle;
 void Alert(const char*,const char*,const char*,const char*){}void SetDeviceState(DeviceState s){state=s;}
 void Schedule(std::function<void()> f){}void Reboot(){}
 bool UpgradeFirmware(const std::string&,const std::string&,const std::string&);
};
@@METHOD@@
int main(){for(bool throws:{false,true}){throw_failure=throws;Application a;
bool ok=a.UpgradeFirmware("https://invalid.test/fw","hutuji.1.0.40",std::string(64,'a'));
if(ok || !a.audio_service_.running || a.state!=kDeviceStateIdle || Board::GetInstance().power!=PowerSaveLevel::LOW_POWER)return 1;}}
'''


def test_ota_exception_restores_runtime():
    compiler = os.environ.get("CXX") or shutil.which("g++")
    assert compiler
    source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
    program = PROGRAM.replace("@@METHOD@@", function(source, "bool Application::UpgradeFirmware("))
    with tempfile.TemporaryDirectory() as folder:
        cpp = Path(folder) / "probe.cpp"
        exe = Path(folder) / ("probe.exe" if os.name == "nt" else "probe")
        cpp.write_text(program, encoding="utf-8")
        built = subprocess.run([compiler, "-std=c++17", "-O0", str(cpp), "-o", str(exe)], capture_output=True, timeout=60)
        assert built.returncode == 0, built.stderr.decode("utf-8", "replace")
        ran = subprocess.run([str(exe)], capture_output=True, timeout=10)
        assert ran.returncode == 0, ran.stderr.decode("utf-8", "replace")
