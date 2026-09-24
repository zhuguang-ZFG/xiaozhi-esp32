import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def find_compiler():
    configured = os.environ.get("CXX", "").strip()

    def ok(path: Path | None) -> Path | None:
        if not path or not path.is_file():
            return None
        resolved = path.resolve()
        # PATH 上的 esp-clang 是交叉工具链，不能拿来链 host 测
        text = str(resolved).lower()
        if "esp-clang" in text or "riscv" in text or "xtensa" in text:
            return None
        return resolved

    candidates = [
        Path(configured) if configured else None,
        Path(found) if (found := shutil.which("g++")) else None,
        Path(found) if (found := shutil.which("clang++")) else None,
        Path("C:/Program Files/LLVM/bin/clang++.exe"),
    ]
    mingw_root = Path.home() / "scoop" / "apps" / "mingw"
    if mingw_root.is_dir():
        candidates.extend(sorted(mingw_root.glob("*/bin/g++.exe"), reverse=True))
    for path in candidates:
        hit = ok(path)
        if hit is not None:
            return hit
    return None


class HutujiMemoryCoreTest(unittest.TestCase):
    def test_unicode_limits_and_strict_restore(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = r'''
        #include "main/boards/lichuang-dev/hutuji_memory_core.h"
        #include <cassert>
        int main() {
            using namespace hutuji::memory;
            std::string key, value;
            for (int i=0; i<51; ++i) key += "猫";
            for (int i=0; i<501; ++i) value += "猫";
            Store store;
            Remember(store, key, value);
            assert(store.entries[0].first.size() == 150);
            assert(store.entries[0].second.size() == 1500);
            assert(FromJsonObject(ToJsonObject(store)).entries == store.entries);
            std::string emoji;
            for (int i=0; i<51; ++i) emoji += "😀";
            StripControlAndClamp(emoji, 50);
            assert(emoji.size() == 200);
            auto decoded = FromJsonObject(R"({"\u732b":"\uD83D\uDE00"})");
            assert(decoded.entries.size() == 1);
            assert(decoded.entries[0].first == "猫" && decoded.entries[0].second == "😀");
            for (const auto* bad : {R"({"a":"b",})", R"({"a":"b"}junk)",
                 R"({"a":"\uZZZZ"})", R"({"a":"\uD800"})", R"({"a":"\uDC00"})",
                 "{\"a\":\"bad\nvalue\"}", "{\"a\":\"\xc0\xaf\"}"}) {
                assert(FromJsonObject(bad).entries.empty());
            }
            auto duplicate = FromJsonObject(R"({"a":"old","a":"new"})");
            assert(duplicate.entries.size() == 1 && duplicate.entries[0].second == "new");
            Store many;
            for (int i=0; i<80; ++i) Remember(many, "topic"+std::to_string(i), "v");
            assert(Recall(many, "topic").find("\"returned\":40") != std::string::npos);
            assert(Recall(many, "topic").find("\"total\":80") != std::string::npos);
        }
        '''
        self._compile_and_run(compiler, source, "memory_unicode")

    def test_nvs_failures_are_reported_without_overwriting(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        stubs = {
            "mcp_server.h": r'''
                #pragma once
                #include <map>
                #include <string>
                #include <functional>
                #include <initializer_list>
                using ReturnValue = std::string;
                constexpr int kPropertyTypeString = 1;
                struct Property {
                    std::string name, text;
                    Property(const char* n, int, std::string t=""):name(n),text(t){}
                    template<class T> T value() const { return text; }
                };
                struct PropertyList {
                    std::map<std::string, Property> items;
                    PropertyList(std::initializer_list<Property> values) {
                        for (const auto& p: values) items.emplace(p.name,p);
                    }
                    const Property& operator[](const char* name) const { return items.at(name); }
                };
                class McpServer {
                public:
                    std::map<std::string,std::function<ReturnValue(const PropertyList&)>> callbacks;
                    void AddTool(const char* name,const char*,PropertyList,
                                 std::function<ReturnValue(const PropertyList&)> cb) { callbacks[name]=cb; }
                };
            ''',
            "nvs.h": r'''
                #pragma once
                #include <string>
                #include <cstring>
                using nvs_handle_t = unsigned;
                using esp_err_t = int;
                constexpr int ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, NVS_READONLY=0, NVS_READWRITE=1;
                inline int fail=0, writes=0, opened=0, reads=0;
                inline std::string persisted=R"({"old":"retained"})", pending;
                inline esp_err_t nvs_open(const char*,int mode,nvs_handle_t* h) {
                    if (fail==1 || (fail==4 && mode==NVS_READWRITE)) return 9;
                    *h=1; ++opened; return 0;
                }
                inline void nvs_close(nvs_handle_t) { --opened; }
                inline esp_err_t nvs_get_str(nvs_handle_t,const char*,char* out,size_t* n) {
                    ++reads;
                    if (fail==2 || (fail==3 && out)) return 9;
                    *n=persisted.size()+1;
                    if (out) std::memcpy(out,persisted.c_str(),*n);
                    return 0;
                }
                inline esp_err_t nvs_set_str(nvs_handle_t,const char*,const char* value) {
                    ++writes; if(fail==5) return 9; pending=value; return 0;
                }
                inline esp_err_t nvs_commit(nvs_handle_t) {
                    if(fail==6) return 9;
                    persisted=pending; return 0;
                }
            ''',
        }
        source = r'''
            #include "main/boards/lichuang-dev/hutuji_memory.cc"
            #include <cassert>
            int main() {
                McpServer server;
                hutuji::memory::RegisterTools(server);
                PropertyList remember({Property("key",1,"new"), Property("value",1,"saved")});
                PropertyList forget({Property("key",1,"old")});
                for (const char* name: {"hutuji.remember","hutuji.forget"}) {
                    for (fail=1; fail<=6; ++fail) {
                        writes=0; pending.clear();
                        auto result=server.callbacks[name](std::string(name)=="hutuji.remember" ? remember : forget);
                        assert(result.find("\"ok\":false") != std::string::npos);
                        assert(persisted == R"({"old":"retained"})");
                        if(fail<=4) assert(writes==0);
                        assert(opened==0);
                    }
                }
                fail=0;
                assert(server.callbacks["hutuji.remember"](remember).find("\"ok\":true")!=std::string::npos);
                assert(persisted.find("saved")!=std::string::npos && opened==0);
                const int previous_writes=writes;
                server.callbacks["hutuji.remember"](remember);
                assert(writes==previous_writes);
                assert(server.callbacks["hutuji.forget"](forget).find("\"ok\":true")!=std::string::npos);
                assert(persisted.find("retained")==std::string::npos && opened==0);
            }
        '''
        self._compile_and_run(compiler, source, "memory_nvs", stubs)

    def _compile_and_run(self, compiler, source, stem, stubs=None):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            source_path = temp / f"{stem}.cpp"
            output_path = temp / (f"{stem}.exe" if os.name == "nt" else stem)
            source_path.write_text(source, encoding="utf-8")
            for name, content in (stubs or {}).items():
                (temp/name).write_text(textwrap.dedent(content), encoding="utf-8")
            build = subprocess.run(
                [
                    str(compiler),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(temp),
                    "-I",
                    str(ROOT),
                    str(source_path),
                    "-o",
                    str(output_path),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(build.returncode, 0, build.stderr or build.stdout)
            env = os.environ.copy()
            env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            run = subprocess.run(
                [str(output_path)],
                cwd=temp,
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )
            self.assertEqual(run.returncode, 0, run.stderr or run.stdout)

    def test_remember_recall_forget_and_isolation_shape(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_memory_core.h"
            #include <cassert>
            #include <string>

            int main() {
                using namespace hutuji::memory;
                Store a;
                assert(Remember(a, "称呼", "小明").find("已记住") != std::string::npos);
                assert(Recall(a, "称呼").find("小明") != std::string::npos);
                Store b;
                assert(Recall(b, "称呼").find("没有记住") != std::string::npos);
                assert(Forget(a, "称呼").find("已忘掉") != std::string::npos);
                assert(Recall(a, "称呼").find("没有记住") != std::string::npos);
                Remember(a, "x", "1");
                Remember(a, "y", "2");
                assert(Forget(a, "*").find("全部忘掉") != std::string::npos);
                assert(Recall(a, "").find("\"total\":0") != std::string::npos);
                // LRU: 写满后最旧被挤
                for (size_t i = 0; i < kMaxEntries; ++i) {
                    Remember(a, "k" + std::to_string(i), "v");
                }
                Remember(a, "extra", "v");
                assert(a.entries.size() == kMaxEntries);
                assert(Recall(a, "k0").find("没有记住") != std::string::npos);
                assert(Recall(a, "extra").find("extra") != std::string::npos);
                // JSON roundtrip
                Store c = FromJsonObject(ToJsonObject(a));
                assert(c.entries.size() == a.entries.size());
                assert(FromJsonObject("{broken").entries.empty());
                assert(kMaxEntries == 80);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_memory_core_test")


if __name__ == "__main__":
    unittest.main()
