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


class PlotterProvisionCoreTest(unittest.TestCase):
    def _compile_and_run(self, compiler, source, stem):
        """把 core 头的纯逻辑编成 host 可执行文件跑一遍，非 0 退出即失败。

        与 test_hutuji_recovery_core.py 同一模式：编译器显式传入，stem 一测一名。
        """
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
            # Scoop/MinGW 的 g++.exe 可直接运行，但新编出的 exe 依赖同目录
            # libstdc++/libgcc DLL；临时目录启动时 Windows 不会自动搜索编译器目录。
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

    def test_command_sequence_matches_serial_script(self):
        """命令集与顺序必须同量产串口脚本 scripts/grbl_wifi_setup.py：只写
        ESP100/101/110 三条 NVS 设置，最后 ESP444 重启生效（写字机固件零改动前提）。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>
            #include <string>

            int main() {
                using namespace hutuji::provision;
                const auto seq = BuildCommandSequence("MyHome", "secret123");
                static_assert(std::tuple_size<decltype(seq)>::value == 4);
                assert(seq[0] == "[ESP100]MyHome");
                assert(seq[1] == "[ESP101]secret123");
                assert(seq[2] == "[ESP110]STA");
                assert(seq[3] == "[ESP444]RESTART");
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_seq")

    def test_url_encode_query_values(self):
        """query 值编码：unreserved（A-Za-z0-9-_.~）原样，其余逐字节 %XX 大写。
        空格不得编成 '+'（Arduino WebServer 的 arg() 只解 %XX）。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>
            #include <string>

            int main() {
                using hutuji::provision::UrlEncode;
                assert(UrlEncode("abcXYZ019-_.~") == "abcXYZ019-_.~");
                assert(UrlEncode("my home") == "my%20home");
                assert(UrlEncode("a&b=c%") == "a%26b%3Dc%25");
                assert(UrlEncode("[ESP100]x") == "%5BESP100%5Dx");
                // 中文 SSID 的 UTF-8 字节逐字节编码（"家" = E5 AE B6）
                assert(UrlEncode("\xE5\xAE\xB6") == "%E5%AE%B6");
                assert(UrlEncode("") == "");
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_urlenc")

    def test_command_url_targets_factory_ap(self):
        """命令 URL 固定打写字机出厂 AP 地址 192.168.0.1 的 /command?plain="""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>
            #include <string>

            int main() {
                using namespace hutuji::provision;
                assert(std::string(kPlotterApHost) == "192.168.0.1");
                assert(std::string(kPlotterApSsid) == "GRBL_ESP");
                assert(std::string(kPlotterApPassword) == "12345678");
                const std::string url = BuildCommandUrl("[ESP110]STA");
                assert(url == "http://192.168.0.1/command?plain=%5BESP110%5DSTA");
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_url")

    def test_classify_response_line_based(self):
        """应答分类口径同 grbl_wifi_setup.py classify_response：error 行优先于任何
        ok 样内容；成功只认独立 `ok` 行或 `:ok` 尾缀——SSID/密码本身可能含 ok 子串，
        子串判定会把失败读成成功并继续发 RESTART。HTTP 侧另认 `Error: <文本>` 形态
        （WebServer.cpp:475 大写 E）与 >=400 状态码。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>

            int main() {
                using hutuji::provision::ClassifyEspResponse;
                using hutuji::provision::EspCmdResult;

                // 成功：独立 ok 行（写命令无输出时 WebServer 的应答体就是 "ok"）
                assert(ClassifyEspResponse(200, "ok") == EspCmdResult::Ok);
                assert(ClassifyEspResponse(200, "ok\r\n") == EspCmdResult::Ok);
                assert(ClassifyEspResponse(200, "OK") == EspCmdResult::Ok);
                assert(ClassifyEspResponse(200, "some output\r\nok\r\n") == EspCmdResult::Ok);

                // 失败：error 行优先（即使同帧里也有 ok 样文本）
                assert(ClassifyEspResponse(200, "Error: Invalid value") == EspCmdResult::Rejected);
                assert(ClassifyEspResponse(200, "error:8") == EspCmdResult::Rejected);
                assert(ClassifyEspResponse(200, "ok\r\nerror: 9") == EspCmdResult::Rejected);
                assert(ClassifyEspResponse(500, "Error: Invalid value") == EspCmdResult::Rejected);
                assert(ClassifyEspResponse(500, "ok") == EspCmdResult::Rejected);

                // 含 ok 子串的回显不得误判成功（SSID "BookCafe"/"Tokyo5G ok" 场景）
                assert(ClassifyEspResponse(200, "[ESP100]Tokyo5G ok") == EspCmdResult::Rejected);
                assert(ClassifyEspResponse(200, "") == EspCmdResult::Rejected);

                // 传输失败（ESP HTTP client 约定 status<=0 = 未拿到响应）
                assert(ClassifyEspResponse(0, "") == EspCmdResult::TransportError);
                assert(ClassifyEspResponse(-1, "") == EspCmdResult::TransportError);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_classify")

    def test_restart_outcome_tolerates_connection_drop(self):
        """ESP444 的 setSystemMode 立即 ESP.restart()（WebSettings.cpp:412-421），
        HTTP 应答可能永远发不出来——传输失败视为已发出；显式被拒才是真失败。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>

            int main() {
                using hutuji::provision::EspCmdResult;
                using hutuji::provision::IsRestartOutcomeAcceptable;
                assert(IsRestartOutcomeAcceptable(EspCmdResult::Ok));
                assert(IsRestartOutcomeAcceptable(EspCmdResult::TransportError));
                assert(!IsRestartOutcomeAcceptable(EspCmdResult::Rejected));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_restart")

    def test_validate_credentials_mirrors_grbl_bounds(self):
        """镜像写字机侧校验边界（WebSettings.cpp StringSetting + WifiConfig.cpp
        isSSIDValid）：SSID 1..32 且全部 ASCII 可打印（中文 SSID 的 UTF-8 字节
        >=0x80 会被 Grbl 拒，本侧先行检出给明确话术）；密码 0（开放）或 8..64。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>
            #include <string>

            int main() {
                using hutuji::provision::CredentialError;
                using hutuji::provision::ValidateHomeCredentials;

                assert(ValidateHomeCredentials("HomeWiFi", "secret123") == CredentialError::None);
                assert(ValidateHomeCredentials("", "secret123") == CredentialError::SsidTooShort);
                assert(ValidateHomeCredentials(std::string(32, 'a'), "secret123") == CredentialError::None);
                assert(ValidateHomeCredentials(std::string(33, 'a'), "secret123") == CredentialError::SsidTooLong);
                // 中文 SSID（UTF-8 多字节）与不可打印字符都过不了 Grbl isPrintable
                assert(ValidateHomeCredentials("\xE5\xAE\xB6\xE9\x87\x8C", "secret123") == CredentialError::SsidNotPrintable);
                assert(ValidateHomeCredentials("a\tb", "secret123") == CredentialError::SsidNotPrintable);

                assert(ValidateHomeCredentials("HomeWiFi", "") == CredentialError::None);  // 开放网络
                assert(ValidateHomeCredentials("HomeWiFi", "1234567") == CredentialError::PasswordLengthInvalid);
                assert(ValidateHomeCredentials("HomeWiFi", "12345678") == CredentialError::None);
                assert(ValidateHomeCredentials("HomeWiFi", std::string(64, 'p')) == CredentialError::None);
                assert(ValidateHomeCredentials("HomeWiFi", std::string(65, 'p')) == CredentialError::PasswordLengthInvalid);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_creds")

    def test_failure_descriptions_and_transience(self):
        """每种失败原因都有中文用户话术；自动重试只救瞬态（跳配失败/重启后未上线/
        回切失败），确定性失败（命令被拒/凭据不合法）不重试、直接转人工。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>
            #include <cstring>

            int main() {
                using namespace hutuji::provision;
                for (int i = 0; i <= static_cast<int>(ProvisionFailure::InvalidCredentials); ++i) {
                    const auto reason = static_cast<ProvisionFailure>(i);
                    assert(DescribeFailure(reason) != nullptr);
                    assert(std::strlen(DescribeFailure(reason)) > 0);
                }
                assert(IsTransient(ProvisionFailure::ApAuthFailed));
                assert(IsTransient(ProvisionFailure::NotOnlineAfterRestart));
                assert(IsTransient(ProvisionFailure::HomeWifiRestoreFailed));
                assert(!IsTransient(ProvisionFailure::ApNotInRange));
                assert(!IsTransient(ProvisionFailure::CommandRejected));
                assert(!IsTransient(ProvisionFailure::InvalidCredentials));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_failure")

    def test_retry_policy_is_bounded(self):
        """自动尝试上限 3 次（防重试风暴）；退避间隔递增且都在分钟级。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/plotter_provision_core.h"
            #include <cassert>

            int main() {
                using namespace hutuji::provision;
                static_assert(kMaxAutoAttempts == 3);
                static_assert(kPatrolDelayMs == 3000);
                static_assert(kJumpConnectTimeoutMs == 10000);
                assert(AutoRetryDelayMs(1) == 60000);
                assert(AutoRetryDelayMs(2) == 180000);
                assert(AutoRetryDelayMs(2) > AutoRetryDelayMs(1));
                // 单次跳配时间盒：跳连 10s + 4 条命令 + 回切等待，必须远在
                // waveshare 断连看门狗 120s 以内，否则回切途中被踹进配网模式。
                static_assert(kJumpConnectTimeoutMs + 4 * kHttpTimeoutMs + kRestoreTimeoutMs
                              < 120000);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "pp_retry")


if __name__ == "__main__":
    unittest.main()
