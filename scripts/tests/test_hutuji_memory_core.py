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
    def _compile_and_run(self, compiler, source, stem):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            source_path = temp / f"{stem}.cpp"
            output_path = temp / (f"{stem}.exe" if os.name == "nt" else stem)
            source_path.write_text(source, encoding="utf-8")
            build = subprocess.run(
                [
                    str(compiler),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
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
