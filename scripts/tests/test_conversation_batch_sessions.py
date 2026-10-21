"""真实队列按连续会话消费，跨会话事件留待下批。"""
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(os.environ.get("XIAOZHI_ROOT", Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(ROOT / "scripts/tests"))
import test_hutuji_aux_lifecycle as aux


class ConversationBatchTest(unittest.TestCase):
    run_cpp = aux.AuxiliaryLifecycleTest.run_cpp

    def test_contiguous_sessions_wraparound_and_remaining_items(self):
        raw = (ROOT / "main/boards/lichuang-dev/hutuji_conversation_report.cc").read_text(encoding="utf-8")
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
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
''' + aux.body(raw, "bool PushTurn(") + aux.body(raw, "size_t PopBatch(") + r'''
int main(){
    PendingTurn out[8];
    for(size_t head=0;head<kQueueCap;++head){
        g_head=head;g_count=0;
        for(auto session:{"A","A","B","A"})assert(PushTurn("user",session,session));
        assert(PopBatch(out,8)==2 && g_count==2);
        assert(std::strcmp(out[0].session_id,"A")==0 && std::strcmp(out[1].session_id,"A")==0);
        assert(PopBatch(out,8)==1 && std::strcmp(out[0].session_id,"B")==0 && g_count==1);
        assert(PopBatch(out,8)==1 && std::strcmp(out[0].session_id,"A")==0 && g_count==0);
        assert(PopBatch(out,8)==0);
        for(int i=0;i<8;++i)assert(PushTurn("user","same","same"));
        assert(PopBatch(out,3)==3 && g_count==5);
        assert(PopBatch(out,8)==5 && g_count==0);
        assert(PopBatch(nullptr,8)==0 && PopBatch(out,0)==0);
    }
}
''')


if __name__ == "__main__":
    unittest.main()
