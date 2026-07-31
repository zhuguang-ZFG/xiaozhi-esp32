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


class HutujiRecoveryCoreTest(unittest.TestCase):
    def test_recovery_decisions_and_abort_reset_token(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstdint>

            int main() {
                using hutuji::AbortResetToken;
                using hutuji::CanSendAbortReset;
                using hutuji::IsStoppedForReset;

                assert(IsStoppedForReset(true, false, false));
                assert(IsStoppedForReset(false, true, true));
                assert(!IsStoppedForReset(false, true, false));
                assert(!IsStoppedForReset(false, false, false));

                bool preconditions[] = {true, true, true, true, true, true};
                assert(CanSendAbortReset(true, true, true, true, true, true));
                for (int missing = 0; missing < 6; ++missing) {
                    preconditions[missing] = false;
                    assert(!CanSendAbortReset(
                        preconditions[0], preconditions[1], preconditions[2],
                        preconditions[3], preconditions[4], preconditions[5]));
                    preconditions[missing] = true;
                }

                AbortResetToken token;
                assert(!token.Pending());
                assert(token.Arm(7));
                assert(token.Pending());
                assert(token.Consume(7));
                assert(!token.Pending());
                assert(!token.Consume(7));

                assert(token.Arm(8));
                token.Cancel();
                assert(!token.Pending());
                assert(!token.Consume(8));

                assert(token.Arm(9));
                assert(!token.Consume(10));
                assert(!token.Pending());
                assert(!token.Consume(9));
                return 0;
            }
            """
        )

        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            source_path = temp / "hutuji_recovery_core_test.cpp"
            output_path = temp / ("hutuji_recovery_core_test.exe" if os.name == "nt" else "hutuji_recovery_core_test")
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
            run = subprocess.run(
                [str(output_path)],
                cwd=temp,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stderr or run.stdout)


if __name__ == "__main__":
    unittest.main()
