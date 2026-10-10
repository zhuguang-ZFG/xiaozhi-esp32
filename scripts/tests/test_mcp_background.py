"""后台工具有界执行；真实调度函数不把网络等待排进主事件循环。"""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get('XIAOZHI_ROOT', Path(__file__).resolve().parents[2]))


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError('函数体未闭合')


class BackgroundToolTest(unittest.TestCase):
    def test_manual_tools_use_background_registration_in_all_three_boards(self):
        boards = ('lichuang-dev/lichuang_dev_board.cc',
                  'waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc',
                  'freenove-esp32s3-display-2.8-lcd/freenove-esp32s3-display-2.8-lcd.cc')
        for board in boards:
            with self.subTest(board=board):
                source = (ROOT / 'main/boards' / board).read_text(encoding='utf-8')
                self.assertTrue(re.search(r'AddBackgroundTool\(\s*"hutuji\.manual"', source), board)
                if 'void ScheduleManualControl(' in source:
                    manual = function_body(source, 'void ScheduleManualControl(')
                    self.assertIn('ScheduleBackground(', manual, board)
        source = (ROOT / 'main/mcp_server.cc').read_text(encoding='utf-8')
        dispatch = function_body(source, 'void McpServer::DoToolCall(')
        self.assertIn('DispatchToolCall(id, *tool_iter, std::move(arguments))', dispatch)

    def test_real_dispatch_limits_workers_and_keeps_main_loop_responsive(self):
        compiler = os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('需要 host C++ 编译器')
        source = (ROOT / 'main/mcp_server.cc').read_text(encoding='utf-8')
        program = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#define ESP_LOGE(...) ((void)0)
using PropertyList = std::vector<int>;
struct McpTool {
    bool in_background = false;
    std::function<std::string(const PropertyList&)> call;
    bool background() const { return in_background; }
    std::string Call(const PropertyList& args) { return call(args); }
};
struct Application {
    std::vector<std::function<void()>> scheduled;
    static Application& GetInstance() { static Application instance; return instance; }
    void Schedule(std::function<void()>&& call) { scheduled.push_back(std::move(call)); }
};
struct Task { void (*entry)(void*); void* arg; };
std::vector<Task> workers;
bool create_ok = true;
constexpr int pdTRUE = 1;
int deleted_tasks = 0, deleted_contexts = 0;
int xTaskCreate(void (*entry)(void*), const char*, int stack, void* arg, int, void*) {
    assert(stack >= 4096 && stack <= 8192);
    if (!create_ok) return 0;
    workers.push_back({entry, arg});
    return pdTRUE;
}
void vTaskDelete(void*) { ++deleted_tasks; }
class McpServer {
public:
    struct BackgroundCall {
        McpServer* server;
        std::function<void()> call;
        ~BackgroundCall() { ++deleted_contexts; }
    };
    std::atomic<bool> background_tool_active_{false};
    std::vector<std::pair<int, std::string>> results, errors;
    void ReplyResult(int id, const std::string& value) { results.emplace_back(id, value); }
    void ReplyError(int id, const std::string& value) { errors.emplace_back(id, value); }
    void DispatchToolCall(int id, McpTool* tool, PropertyList arguments);
    bool ScheduleBackground(std::function<void()> call);
    static void BackgroundToolTaskEntry(void* arg);
};
''' + function_body(source, 'void McpServer::DispatchToolCall(') + '\n' + function_body(source, 'bool McpServer::ScheduleBackground(') + '\n' + function_body(source, 'void McpServer::BackgroundToolTaskEntry(') + r'''
int main() {
    McpServer server;
    auto& app = Application::GetInstance();
    int calls = 0;
    McpTool tool;
    tool.call = [&](const PropertyList& args) {
        ++calls;
        assert(args.size() == 1 && args[0] == 7);
        return std::string("完成");
    };
    server.DispatchToolCall(1, &tool, {7});
    assert(app.scheduled.size() == 1 && workers.empty() && calls == 0);
    app.scheduled.back()(); app.scheduled.clear();
    assert(server.results.back().first == 1 && calls == 1);
    tool.in_background = true;
    server.DispatchToolCall(2, &tool, {7});
    assert(workers.size() == 1 && app.scheduled.empty() && calls == 1);
    server.DispatchToolCall(3, &tool, {7});
    assert(workers.size() == 1 && server.errors.back().first == 3);
    bool status_ran = false;
    app.Schedule([&]() { status_ran = true; });
    app.scheduled.back()(); app.scheduled.clear();
    assert(status_ran && calls == 1);
    auto task = workers.back(); workers.clear(); task.entry(task.arg);
    assert(server.results.back().first == 2 && calls == 2);
    assert(!server.background_tool_active_ && deleted_tasks == 1 && deleted_contexts == 1);
    tool.call = [](const PropertyList&) -> std::string { throw std::runtime_error("测试异常"); };
    server.DispatchToolCall(4, &tool, {7});
    task = workers.back(); workers.clear(); task.entry(task.arg);
    assert(server.errors.back().first == 4 && !server.background_tool_active_);
    assert(deleted_tasks == 2 && deleted_contexts == 2);
    create_ok = false;
    server.DispatchToolCall(5, &tool, {7});
    assert(workers.empty() && !server.background_tool_active_ && server.errors.back().first == 5);
    assert(deleted_contexts == 3);
    create_ok = true;
    server.DispatchToolCall(6, &tool, {7});
    task = workers.back(); workers.clear(); task.entry(task.arg);
    assert(server.errors.back().first == 6 && !server.background_tool_active_);
    assert(deleted_tasks == 3 && deleted_contexts == 4);
    assert(server.ScheduleBackground([]() { throw 7; }));
    assert(!server.ScheduleBackground([]() {}));
    task = workers.back(); workers.clear(); task.entry(task.arg);
    assert(!server.background_tool_active_ && deleted_tasks == 4 && deleted_contexts == 5);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            stem = Path(directory) / 'mcp_background'
            stem.with_suffix('.cpp').write_text(program, encoding='utf-8')
            exe = stem.with_suffix('.exe' if os.name == 'nt' else '')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(stem.with_suffix('.cpp')), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr or result.stdout)


if __name__ == '__main__':
    unittest.main()
