"""编译真实停止入口、断流归位和任务尾段，核对失败不能发布已停止。"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]

def compile_probe():
    spec = importlib.util.spec_from_file_location('nopaper', ROOT / 'scripts/tests/test_hutuji_nopaper_core.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    case = module.HutujiNopaperCoreTest()
    capture = {}
    case._compile_and_run = lambda compiler, source, stem: capture.update(source=source)
    case._compile_abort_harness('int main() { return 0; }', 'capture')
    raw = (ROOT / 'main/boards/lichuang-dev/hutuji_job.cc').read_text(encoding='utf-8')
    def function(signature):
        start = raw.index(signature)
        left = raw.index('{', start)
        depth, end = 1, left + 1
        while depth:
            depth += (raw[end] == '{') - (raw[end] == '}')
            end += 1
        return raw[start:end]
    source = capture['source']
    source = source.replace('    RunQueuedWorker();', '    if (pending) RunQueuedWorker();')
    source = source.replace('enum class GrblState { Idle };', '''
    int scenario = 0, idle_calls = 0, sends = 0, closes = 0;
    bool connected = true;
    std::string last_notify;
    enum class GrblState { Idle, Hold, Cycle };
    enum class WaitResult { Ok, Error };
    constexpr int kAbortDrainQuiescenceTimeoutMs = 1000, kAbortDrainIdleTimeoutMs = 1000;
    constexpr int kHomeOkTimeoutMs = 1000, kPenOriginIdleTimeoutMs = 1000;
    constexpr int kPenSpringReturnMs = 0, kHomeIdleTimeoutMs = 1000;
    ''')
    source = source.replace('bool IsConnected() { return true; }', '''
    bool IsConnected() { return connected; }
    bool IsReady() { return connected; }
    bool IsNopaperMachine() { return true; }
    uint32_t GetConnectionSequence() { return 7; }
    uint32_t GetResetBannerSequence() { return 9; }
    bool SendResumeForSession(uint32_t, uint32_t) { return true; }
    bool SendLineForSession(const char*, uint32_t, uint32_t) { ++sends; return !(scenario == 2); }
    WaitResult WaitResponse(int, void*, int*) { return WaitResult::Ok; }
    ''')
    source = source.replace('void ShutdownSocket(uint32_t) { assert(false && "unexpected wait timeout"); }',
                            'void ShutdownSocket(uint32_t) { ++closes; connected = false; }')
    source = source.replace('void Notify(const char*) {}', 'void Notify(const char* text) { last_notify = text; }')
    begin = source.index('    bool PerformAbortDrainHome() {')
    end = source.index('    std::string RequestAbort();', begin)
    source = source[:begin] + '''
        std::atomic<hutuji::StreamQuiescence> stream_quiescence_{hutuji::StreamQuiescence::Quiesced};
        bool PerformAbortDrainHome();
        bool StartAbortResetTask() { return false; }
        void SetStreamingOrPaused() {}
        bool WaitForIdleForSession(bool, int, uint32_t, uint32_t) {
            ++idle_calls;
            return !(scenario == 1 && idle_calls == 1) && !(scenario == 3 && idle_calls == 3);
        }
        bool ReleaseMotorsAfterHome(uint32_t, uint32_t, bool) { return scenario != 4; }
    ''' + source[end:]
    source = source.replace('int main() { return 0; }', '')
    source += '\nusing hutuji::CanResetAfterStream;\n' + function('bool Job::PerformAbortDrainHome()').replace('std::mutex', 'HookMutex')
    source += r'''
    int main(int argc, char** argv) {
        scenario = argc > 1 ? std::atoi(argv[1]) : 0;
        auto& job = Job::GetInstance();
        const auto reply = job.RequestAbort();
        job.RunTail(false);
        std::printf("{\"scenario\":%d,\"reply\":\"%s\",\"state\":\"%s\",\"homed\":%s,\"connected\":%s,\"busy\":%s,\"idle_calls\":%d,\"sends\":%d,\"closes\":%d,\"notify\":\"%s\"}\n",
           scenario, reply.c_str(), job.state_.c_str(), job.abort_home_done_.load()?"true":"false",
           connected?"true":"false", job.busy_.load()?"true":"false",idle_calls,sends,closes,last_notify.c_str());
        return 0;
    }
    '''
    return source

class AbortDrainFailureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = os.environ.get('CXX') or shutil.which('g++')
        if not cls.compiler:
            raise unittest.SkipTest('需要宿主 C++ 编译器')
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cpp = Path(cls.folder.name)/'probe.cpp'
        cpp.write_text(compile_probe(),encoding='utf-8')
        cls.exe = cpp.with_suffix('.exe' if os.name=='nt' else '')
        cls.env = dict(os.environ, PATH=str(Path(cls.compiler).parent)+os.pathsep+os.environ.get('PATH',''))
        build=subprocess.run([cls.compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT),str(cpp),'-o',str(cls.exe)],env=cls.env,capture_output=True,timeout=60)
        if build.returncode:
            raise AssertionError(build.stderr.decode('utf-8','replace'))

    def run_case(self,scenario):
        run=subprocess.run([str(self.exe),str(scenario)],env=self.env,capture_output=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stderr.decode('utf-8','replace'))
        return json.loads(run.stdout)

    def test_success_waits_for_home_and_preserves_success_notice(self):
        result=self.run_case(0)
        self.assertEqual(result['state'],'aborted')
        self.assertTrue(result['homed'])
        self.assertEqual(result['notify'],'已停止并回原点')

    def check_failure(self,scenario):
        result=self.run_case(scenario)
        self.assertEqual(result['state'],'error')
        self.assertFalse(result['homed'])
        self.assertFalse(result['busy'])
        self.assertFalse(result['connected'])
        self.assertIn('取消失败',result['notify'])

    def test_unconfirmed_idle_is_not_reported_as_stopped(self): self.check_failure(1)
    def test_send_failure_is_not_reported_as_stopped(self): self.check_failure(2)
    def test_home_timeout_is_not_reported_as_stopped(self): self.check_failure(3)
    def test_motor_release_failure_is_not_reported_as_stopped(self): self.check_failure(4)

if __name__=='__main__': unittest.main()
