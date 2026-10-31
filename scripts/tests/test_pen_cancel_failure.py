"""真实试笔入口验证应答失败和取消交错。"""
import importlib.util,json,os,subprocess,shutil,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def program():
    spec=importlib.util.spec_from_file_location('pen',ROOT/'scripts/tests/test_hutuji_pen_session.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    source=module.program();source=source[:source.index('int main(')]
    source=source.replace('int xTaskCreate(void(*fn)(void*),const char*,int,void* arg,int,void*){fn(arg);return pdTRUE;}', 'int xTaskCreate(void(*fn)(void*),const char*,int,void* arg,int,void*){if(scenario==99)JobCancelBefore();fn(arg);return pdTRUE;}')
    source=source.replace('int lock_count=0;', 'void JobCancelBefore();\nint lock_count=0;')
    source=source.replace('WaitResult TakeResponse(uint32_t){return WaitResult::Ok;}', 'WaitResult TakeResponse(uint32_t);')
    source=source.replace('void Notify(const std::string&){}','std::string notice;\n    void Notify(const std::string& message){notice=message;}')
    source+=r'''
    int scenario_kind=0;
    WaitResult Pipe::TakeResponse(uint32_t){
        if(scenario_kind==0 || scenario_kind==2){
            Job::GetInstance().abort_requested_.store(true);
            if(scenario_kind==0) connected_.store(false);
            return WaitResult::Timeout;
        }
        return WaitResult::Ok;
    }
    int main(int argc,char**argv){
        inject=argc==1;
        scenario=0;test_mode=5;
        auto& job=Job::GetInstance();auto& pipe=Pipe::GetInstance();
        const auto reply=job.RequestPenTest();
        std::printf("{\"inject_failure\":%s,\"reply\":\"%s\",\"state\":\"%s\",\"notice\":\"%s\",\"busy\":%s,\"connected\":%s,\"sent_commands\":%zu}\n",
          inject?"true":"false",reply.c_str(),job.state_.c_str(),job.notice.c_str(),job.busy_.load()?"true":"false",pipe.connected_.load()?"true":"false",pipe.commands.size());
    }
    '''
    begin=source.index('int main(int argc,char**argv)')
    source=source[:begin]+r"""
void JobCancelBefore(){Job::GetInstance().abort_requested_.store(true);}
int main(int argc,char**argv){
    scenario_kind=argc>1?std::atoi(argv[1]):0;
    scenario=scenario_kind==3?99:0;test_mode=5;
    auto& job=Job::GetInstance();auto& pipe=Pipe::GetInstance();
    job.RequestPenTest();
    if(scenario_kind==0 || scenario_kind==2){
        assert(job.state_=="error");assert(job.notice.find("未确认")!=std::string::npos);
    }else if(scenario_kind==3){assert(job.state_=="aborted"&&pipe.commands.empty());}
    else{assert(job.state_=="done");}
    assert(!job.busy_.load()&&!job.pen_test_active_.load());
}
"""
    return source

class PenCancelFailureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=os.environ.get('CXX') or shutil.which('g++')
        if not compiler:raise unittest.SkipTest('需要host C++编译器')
        cls.folder=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.folder.cleanup)
        cpp=Path(cls.folder.name)/'pen.cpp';cpp.write_text(program(),encoding='utf-8')
        cls.exe=cpp.with_suffix('.exe' if os.name=='nt' else '')
        cls.env=dict(os.environ,PATH=str(Path(compiler).parent)+os.pathsep+os.environ.get('PATH',''))
        build=subprocess.run([compiler,'-std=c++17','-I',str(ROOT),str(cpp),'-o',str(cls.exe)],env=cls.env,capture_output=True,timeout=60)
        if build.returncode:raise AssertionError(build.stderr.decode('utf-8','replace'))
    def check(self,scenario):
        run=subprocess.run([str(self.exe),str(scenario)],env=self.env,capture_output=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stderr.decode('utf-8','replace'))
    def test_cancel_after_motion_ack_loss_stays_error(self):self.check(0)
    def test_normal_pen_test_still_completes(self):self.check(1)
    def test_cancel_with_bad_ack_even_if_idle_stays_error(self):self.check(2)
    def test_cancel_before_motion_is_success_without_motion(self):self.check(3)
