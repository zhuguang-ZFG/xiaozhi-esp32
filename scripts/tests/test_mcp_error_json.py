"""真实 cJSON 验证设备错误响应的引号、换行和内存失败回退。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get('XIAOZHI_ROOT', Path(__file__).resolve().parents[2]))

class McpErrorJsonTest(unittest.TestCase):
    def test_error_reply_remains_valid_json(self):
        compiler = os.environ.get('CXX') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要 host C++ 编译器')
        dep = Path(os.environ.get('CJSON_ROOT', ROOT / 'managed_components/espressif__cjson/cJSON'))
        source = (ROOT / 'main/mcp_server.cc').read_text(encoding='utf-8')
        start = source.index('void McpServer::ReplyError(')
        end = source.index('\n}', start) + 2
        program = r'''
#include "cJSON.h"
#include <cassert>
#include <cstdlib>
#include <string>
struct Application {
    std::string message;
    static Application& GetInstance() { static Application app; return app; }
    void SendMcpMessage(const std::string& value) { message=value; }
};
struct McpServer { void ReplyError(int id,const std::string& value); };
''' + source[start:end] + r'''
int main() {
    McpServer server;
    for (const std::string& message : {std::string("普通错误"), std::string("名字\"反斜线\\换行\n\t\r")}) {
        server.ReplyError(7,message);
        auto* result=cJSON_Parse(Application::GetInstance().message.c_str());
        assert(result);
        assert(cJSON_GetObjectItem(result,"id")->valueint==7);
        auto* value=cJSON_GetObjectItem(cJSON_GetObjectItem(result,"error"),"message");
        assert(cJSON_IsString(value) && message==value->valuestring);
        cJSON_Delete(result);
    }
    cJSON_Hooks hooks{[](size_t)->void* { return nullptr; },free};
    cJSON_InitHooks(&hooks);
    server.ReplyError(8,"无法分配 JSON");
    cJSON_InitHooks(nullptr);
    auto* result=cJSON_Parse(Application::GetInstance().message.c_str());
    assert(result && cJSON_GetObjectItem(result,"id")->valueint==8);
    cJSON_Delete(result);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'mcp_error.cpp'
            exe = Path(directory) / ('mcp_error.exe' if os.name == 'nt' else 'mcp_error')
            cpp.write_text(program, encoding='utf-8')
            built = subprocess.run([compiler, '-std=c++17', str(cpp), str(dep/'cJSON.c'),
                                    '-I'+str(dep), '-o', str(exe)], capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, (built.stdout+built.stderr).decode('utf-8','replace'))
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH','')
            result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, (result.stdout+result.stderr).decode('utf-8','replace'))

if __name__ == '__main__':
    unittest.main()
