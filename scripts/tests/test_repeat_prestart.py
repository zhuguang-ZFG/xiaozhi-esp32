"""真实重画/停止入口验证预启动取消与任务创建失败。"""
import importlib.util,json,os,subprocess,shutil,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def program():
    spec=importlib.util.spec_from_file_location('nopaper',ROOT/'scripts/tests/test_hutuji_nopaper_core.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    case=module.HutujiNopaperCoreTest();captured={}
    case._compile_and_run=lambda compiler,source,stem:captured.update(source=source)
    case._compile_abort_harness('int main(){return 0;}','capture')
    source=captured['source']
    raw=(ROOT/'main/boards/lichuang-dev/hutuji_job.cc').read_text(encoding='utf-8')
    def function(signature):
        start=raw.index(signature);left=raw.index('{',start);depth,end=1,left+1
        while depth:
            depth+=(raw[end]=='{')-(raw[end]=='}');end+=1
        return raw[start:end]
    source=source.replace('using BaseType_t = int;', '''
    struct PaperConfig {};
    std::string PaperMarker(const PaperConfig&){return "paper";}
    void InjectAbort();
    bool inject_cancel = true;
    constexpr int pdTRUE=1,MALLOC_CAP_INTERNAL=0;
    unsigned heap_caps_get_free_size(int){return 10000;}
    unsigned heap_caps_get_largest_free_block(int){return 10000;}
    #define ESP_LOGE(...) ((void)0)
    using BaseType_t = int;''')
    source=source.replace('const char*, int, void*, int, void*) {','const char* name, int, void*, int, void*) {')
    source=source.replace('    ++create_calls;\n    if (creation_succeeds)', '    ++create_calls;\n    if (std::string(name)=="hutuji_draw") return 0;\n    if (creation_succeeds)')
    source=source.replace('if (std::string(name)=="hutuji_draw") return 0;', 'if (std::string(name)=="hutuji_draw") return creation_succeeds ? 1 : 0;')
    source=source.replace('    RunQueuedWorker();','    if (delay==50 && inject_cancel) InjectAbort();')
    source=source.replace('bool IsConnected() { return true; }','bool IsConnected() { return true; }\n    bool IsReady() { return true; }')
    source=source.replace('    std::string RequestAbort();','''
        std::atomic<hutuji::StreamQuiescence> stream_quiescence_{hutuji::StreamQuiescence::Idle};
        size_t buffer_len_=1;
        std::string url_="cached.gcode",job_paper_marker_="paper",buffer_paper_marker_="paper";
        std::vector<std::pair<std::string,std::string>> article_pages_;
        size_t article_index_=0;
        std::atomic<unsigned> article_page_{0};
        std::atomic<bool> preview_worker_active_{false},abort_hold_confirmed_{false};
        bool GetPaperConfig(PaperConfig&){return true;}
        static void TaskEntry(void*){}
        std::string RequestRepeat();
        std::string RequestAbort();''')
    source=source.replace('int main(){return 0;}','')
    source+='\nusing hutuji::StreamQuiescence;\n'+function('std::string Job::RequestRepeat()').replace('std::mutex','HookMutex')
    source+=r'''
    void InjectAbort(){
        auto& job=Job::GetInstance();
        assert(job.RequestAbort()=="ok");
        assert(job.busy_.load());
    }
    int main(){
        auto& job=Job::GetInstance();
        char cached='x';job.buffer_=&cached;
        job.busy_.store(false);job.buffer_replayable_.store(true);
        const auto reply=job.RequestRepeat();
        std::printf("{\"reply\":\"%s\",\"busy\":%s,\"abort_worker_active\":%s,\"abort_requested\":%s,\"state\":\"%s\",\"creates\":%d,\"pending_worker\":%s}\n",
          reply.c_str(),job.busy_.load()?"true":"false",job.abort_reset_worker_active_.load()?"true":"false",
          job.abort_requested_.load()?"true":"false",job.state_.c_str(),create_calls,pending?"true":"false");
    }
    '''
    begin=source.index('int main(){')
    source=source[:begin]+r"""
int main(int argc,char**argv){
    const int scenario=argc>1?std::atoi(argv[1]):0;
    auto& job=Job::GetInstance();char cached='x';job.buffer_=&cached;
    job.busy_.store(false);job.buffer_replayable_.store(true);
    inject_cancel=scenario==0 || scenario==1;
    creation_succeeds=scenario!=0 && scenario!=2;
    const auto reply=job.RequestRepeat();
    if(inject_cancel){
        assert(reply=="重画已取消");
        assert(create_calls==0&&pending==nullptr);
        assert(!job.busy_.load()&&!job.abort_requested_.load()&&!job.abort_reset_worker_active_.load());
        assert(!job.buffer_replayable_.load()&&!job.repeat_mode_.load()&&job.state_=="aborted");
    }else if(scenario==2){
        assert(reply.find("error")!=std::string::npos&&create_calls==1);
        assert(!job.busy_.load()&&!job.abort_reset_worker_active_.load()&&job.state_=="idle");
    }else{
        assert(reply=="started"&&create_calls==1&&job.busy_.load());
        assert(job.repeat_mode_.load()&&job.state_=="streaming");
    }
}
"""
    return source

class RepeatPrestartTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=os.environ.get('CXX') or shutil.which('g++')
        if not compiler:raise unittest.SkipTest('需要host C++编译器')
        cls.folder=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.folder.cleanup)
        cpp=Path(cls.folder.name)/'repeat.cpp';cpp.write_text(program(),encoding='utf-8')
        cls.exe=cpp.with_suffix('.exe' if os.name=='nt' else '')
        cls.env=dict(os.environ,PATH=str(Path(compiler).parent)+os.pathsep+os.environ.get('PATH',''))
        build=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT),str(cpp),'-o',str(cls.exe)],env=cls.env,capture_output=True,timeout=60)
        if build.returncode:raise AssertionError(build.stderr.decode('utf-8','replace'))
    def check(self,scenario):
        run=subprocess.run([str(self.exe),str(scenario)],env=self.env,capture_output=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stderr.decode('utf-8','replace'))
    def test_cancel_before_failed_draw_create_creates_no_worker(self):self.check(0)
    def test_cancel_before_successful_draw_create_creates_no_worker(self):self.check(1)
    def test_normal_create_failure_releases_only_its_own_slot(self):self.check(2)
    def test_normal_replay_still_creates_one_draw_worker(self):self.check(3)
