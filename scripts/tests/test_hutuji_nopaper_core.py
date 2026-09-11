"""hutuji_nopaper_core.h 的 host 侧行为钉：VER 机型识别、多页闸、页尾跳过、
$$ 指纹分表（§10.4.15 量产无换纸 SKU）。

与 test_hutuji_recovery_core.py 同法：把纯逻辑编成 host 可执行文件跑行为断言，
非 0 退出即失败。编译器缺失时整个用例类 skip（与仓内既有 core 测试同口径）。
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def find_compiler():
    configured = os.environ.get("CXX", "").strip()
    candidates = [
        Path(configured) if configured else None,
        Path(found) if (found := shutil.which("clang++")) else None,
        Path("C:/Program Files/LLVM/bin/clang++.exe"),
        Path(found) if (found := shutil.which("g++")) else None,
    ]
    mingw_root = Path.home() / "scoop" / "apps" / "mingw"
    if mingw_root.is_dir():
        candidates.extend(sorted(mingw_root.glob("*/bin/g++.exe"), reverse=True))
    return next((path.resolve() for path in candidates if path and path.is_file()), None)


class HutujiNopaperCoreTest(unittest.TestCase):
    def _compile_and_run(self, compiler, source, stem):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            source_path = temp / f"{stem}.cpp"
            output_path = temp / (f"{stem}.exe" if os.name == "nt" else stem)
            source_path.write_text(source, encoding="utf-8")
            build = subprocess.run(
                [
                    str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT), str(source_path), "-o", str(output_path),
                ],
                cwd=ROOT, capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(build.returncode, 0, build.stderr or build.stdout)
            run = subprocess.run([str(output_path)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stderr or run.stdout)

    def test_ver_line_sku_detection(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <string>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"

int main() {
    using hutuji::GrblVerLineIsNopaperSku;
    // 换纸机 / 无换纸机
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.20211103:]"));
    assert(GrblVerLineIsNopaperSku("[VER:1.3a.20260910:]"));
    // build 段精确匹配：前缀/子串陷阱一律不认
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.12026091:]"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.2026091:]"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.202609100:]"));
    // 带尾缀仍取 build 段
    assert(GrblVerLineIsNopaperSku("[VER:1.3a.20260910:custom]"));
    // 非 VER 行 / 残缺行 / 空行：保守不认
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a"));
    assert(!GrblVerLineIsNopaperSku("[VER:1.3a.]"));
    assert(!GrblVerLineIsNopaperSku("Grbl 1.3a ['$' for help]"));
    assert(!GrblVerLineIsNopaperSku(""));
    return 0;
}
''', "nopaper_ver")

    def test_behaviour_predicates(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <cstring>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"

int main() {
    using namespace hutuji;
    // 页尾跳过：仅无换纸机
    assert(NopaperSkipsPaperChange(true));
    assert(!NopaperSkipsPaperChange(false));
    // 多页闸：无换纸机 pages>1 拒；单页放行；换纸机全放行
    assert(NopaperRejectsMultiPage(true, 2));
    assert(NopaperRejectsMultiPage(true, 20));
    assert(!NopaperRejectsMultiPage(true, 1));
    assert(!NopaperRejectsMultiPage(true, 0));
    assert(!NopaperRejectsMultiPage(false, 20));
    // 拒绝话术字面量（与 §10.4.15 一致，改文案须同步契约）
    assert(std::strcmp(kNopaperMultiPageRejectMsg,
                       "这台机器一次只能画一页哦") == 0);
    return 0;
}
''', "nopaper_predicates")

    def test_golden_table_split(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("无 host C++ 编译器")
        self._compile_and_run(compiler, r'''
#include <cassert>
#include <cstring>
#include "main/boards/lichuang-dev/hutuji_nopaper_core.h"

int main() {
    using namespace hutuji;
    const GrblSettingGolden* table = nullptr;
    size_t count = 0;
    // 换纸机：13 项含长名 Verbose 锁表项
    ActiveGrblSettingGoldens(false, table, count);
    assert(count == kGrblSettingGoldenCount);
    bool has_verbose = false;
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(table[i].response_key, "Errors/Verbose") == 0) has_verbose = true;
    }
    assert(has_verbose);
    // 无换纸机：12 项、无长名项；$110/$111 于 2026-09-11 拍板提速 12000（余项=机头默认）
    ActiveGrblSettingGoldens(true, table, count);
    assert(count == kGrblSettingGoldenNopaperCount);
    assert(count == 12);
    for (size_t i = 0; i < count; ++i) {
        assert(std::strcmp(table[i].response_key, "Errors/Verbose") != 0);
    }
    assert(table[0].expected == 255.0 && table[0].integer);   // $1 弹簧笔常使能
    assert(table[1].expected == 4.0 && table[1].integer);     // $3 只反 Z（bit Z=4）
    assert(table[7].expected == 12000.0);                     // $110 提速后限速（2026-09-11）
    assert(table[8].expected == 12000.0);                     // $111
    assert(table[11].expected == 20.0);                       // $132 笔程
    return 0;
}
''', "nopaper_goldens")


if __name__ == "__main__":
    unittest.main()
