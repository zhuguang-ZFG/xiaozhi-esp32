"""真实试笔与错误发布回归，保留本次应答失败。"""
import importlib.util,json,os,subprocess,shutil,tempfile,unittest
from pathlib import Path
ROOT=Path(os.environ.get('XIAOZHI_ROOT',Path(__file__).resolve().parents[2]))
def module(name,file):
 spec=importlib.util.spec_from_file_location(name,file);result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
def function(raw,signature):
 start=raw.index(signature);left=raw.index('{',start);depth,end=1,left+1
 while depth:depth+=(raw[end]=='{')-(raw[end]=='}');end+=1
 return raw[start:end]
def run(name,source,scenarios):return source
def program():
    fixture = module('pen', ROOT/'scripts/tests/test_hutuji_pen_session.py')
    source = fixture.program().split('int main(')[0]
    source = source.replace('WaitResult TakeResponse(uint32_t){return WaitResult::Ok;}', 'WaitResult TakeResponse(uint32_t){return scenario==1?WaitResult::Timeout:WaitResult::Ok;}')
    raw = (ROOT/'main/boards/lichuang-dev/hutuji_job.cc').read_text(encoding='utf-8')
    source = source.replace('struct Display {void SetStatus(const char*){}};', 'struct Display {virtual ~Display()=default;void SetStatus(const char*){}};\nstruct LvglDisplay:Display{void UpdateMachineControlState(const std::string&) {}};')
    source = source.replace('void SetState(const std::string& s){state_=s;}', '''
    std::string status_error_mirror_,manual_error_;
    std::atomic<bool> self_heal_pending_{false},manual_pen_down_latched_{false};
    uint32_t draw_start_tick_=0;
    void StartSelfHealTask(){}
    void SetState(const char* s);''')
    source = source.replace('void Notify(const std::string&){}', 'std::string notice;void Notify(const std::string& value){notice=value;}')
    source += '\n#include <cstring>\nconstexpr int kTlsHeapFailSelfHealThreshold=2;\n' + function(raw, 'void Job::SetState(const char* state)')
    source += r'''
int main(int argc,char**argv){
 const int mode=argc>1?std::atoi(argv[1]):0;scenario=mode;test_mode=5;
 auto& job=Job::GetInstance();auto& p=Pipe::GetInstance();
 if(scenario==2 || scenario==3)job.last_error_="old-draw-error";
 if(scenario==3)scenario=1;
 job.RequestPenTest();
 std::cout<<"{\"scenario\":"<<mode<<",\"state\":\""<<job.state_<<"\",\"error\":\""<<job.status_error_mirror_
 <<"\",\"last_error\":\""<<job.last_error_<<"\",\"notice\":\""<<job.notice<<"\",\"commands\":"<<p.commands.size()<<"}";
 assert(job.state_==(scenario==1?"error":"done"));
 if(mode==1)assert(job.status_error_mirror_.find("试笔运动应答超时")!=std::string::npos);
 if(mode==3)assert(job.status_error_mirror_.find("试笔运动应答超时")!=std::string::npos);
 if(mode==2)assert(job.last_error_.empty());
}
'''
    return run('s3-pen-error', source, [0, 1, 2, 3])


class PenErrorStateTest(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  compiler=os.environ.get('CXX') or shutil.which('g++')
  if not compiler:raise unittest.SkipTest('需要host C++编译器')
  cls.folder=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.folder.cleanup)
  cpp=Path(cls.folder.name)/'pen.cpp';cpp.write_text(program(),encoding='utf-8');cls.exe=cpp.with_suffix('.exe' if os.name=='nt' else '')
  cls.env=dict(os.environ,PATH=str(Path(compiler).parent)+os.pathsep+os.environ.get('PATH',''))
  built=subprocess.run([compiler,'-std=c++17','-O0',str(cpp),'-o',str(cls.exe)],env=cls.env,capture_output=True,timeout=60)
  if built.returncode:raise AssertionError(built.stderr.decode('utf-8','replace'))
 def check(self,mode):
  ran=subprocess.run([str(self.exe),str(mode)],env=self.env,capture_output=True,timeout=10)
  self.assertEqual(ran.returncode,0,ran.stderr.decode('utf-8','replace'))
 def test_normal_pen_has_no_error(self):self.check(0)
 def test_ack_timeout_publishes_current_error(self):self.check(1)
 def test_success_clears_previous_error(self):self.check(2)
 def test_ack_timeout_does_not_publish_previous_error(self):self.check(3)
