"""真实配网函数连续调用不复用前次SSID和密码。"""
import os,subprocess,tempfile,unittest
from pathlib import Path
from test_hutuji_ota_reservation import function
ROOT=Path(__file__).resolve().parents[2]
STUB='\n#include <atomic>\n#include <string>\n#include <vector>\n#include <iostream>\n#include "D:/Users/xiaozhi-esp32/main/boards/lichuang-dev/plotter_provision_core.h"\n#define ESP_LOGW(...)\n#define ESP_LOGI(...)\n#define pdMS_TO_TICKS(x) (x)\nvoid vTaskDelay(unsigned){}\nstruct WifiManager{\n std::string ssid="Home-A";int stops=0;\n static WifiManager& GetInstance(){static WifiManager value;return value;}\n std::string GetSsid(){return ssid;}void StartStation(){}void StopStation(){++stops;}\n};\nstruct SsidManager{\n struct Item{std::string ssid,password;};std::vector<Item> list;\n static SsidManager& GetInstance(){static SsidManager value;return value;}\n std::vector<Item> GetSsidList(){return list;}\n};\nnamespace hutuji{\nstruct Pipe{static Pipe& GetInstance(){static Pipe p;return p;}bool IsConnected(){return false;}};\nconstexpr unsigned kGracePollMs=1000,kVerifyPollMs=2000;\nstruct PlotterProvision{\n std::string home_ssid_,home_password_;int auto_attempts_=0;std::atomic<bool>busy_{false},task_active_{true};\n bool visible=false;std::vector<std::string> sent;\n void TeardownJump(){}bool WaitHomeBack(){return true;}\n bool IsFactoryApVisible(){return visible;}bool JumpToFactoryAp(){return true;}\n bool SendCommands(provision::ProvisionFailure*){sent.push_back(home_ssid_);return false;}\n void FinishWith(bool,bool,provision::ProvisionFailure,int){}\n void ProvisionTask(bool);\n};\n'
class ProvisionCredentialsTest(unittest.TestCase):
 def test_missing_current_ssid_cannot_reuse_old(self):
  source=STUB+function((ROOT/'main/boards/lichuang-dev/plotter_provision.cc').read_text(encoding='utf-8'),'void PlotterProvision::ProvisionTask(')+r'''
}
#include <cassert>
int main(){
 auto& wifi=WifiManager::GetInstance();auto& list=SsidManager::GetInstance().list;
 list={{"Home-A","old-password"}};hutuji::PlotterProvision p;p.ProvisionTask(true);
 assert(p.sent.empty());
 wifi.ssid="Home-B";list.clear();p.visible=true;p.ProvisionTask(true);
 assert(p.sent.empty() && p.home_ssid_.empty() && p.home_password_.empty());
 list={{"Home-B","new-password"}};p.ProvisionTask(true);
 assert(p.sent.size()==1 && p.sent[0]=="Home-B" && p.home_password_=="new-password");
}
'''
  with tempfile.TemporaryDirectory() as d:
   cpp=Path(d)/'case.cpp';cpp.write_text(source,encoding='utf-8');exe=cpp.with_suffix('.exe')
   r=subprocess.run([os.environ['CXX'],'-std=c++17',str(cpp),'-o',str(exe)],capture_output=True,timeout=60)
   self.assertEqual(r.returncode,0,r.stderr.decode('utf-8','replace'))
   r=subprocess.run([str(exe)],capture_output=True,timeout=10)
   self.assertEqual(r.returncode,0,r.stderr.decode('utf-8','replace'))
