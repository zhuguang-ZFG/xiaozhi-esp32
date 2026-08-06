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
    def _compile_and_run(self, compiler, source, stem):
        """把 core 头的纯逻辑编成 host 可执行文件跑一遍，非 0 退出即失败。

        `compiler` 与 `stem` 都不给默认值：编译器显式传入，漏传即在调用点报
        TypeError，而不是靠隐式实例属性（漏赋值时错误会指向本 helper 而非漏的地方）；
        stem 每个测试各自命名，避免默认值掩盖「一测一名」的意图。
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
            run = subprocess.run(
                [str(output_path)],
                cwd=temp,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(run.returncode, 0, run.stderr or run.stdout)

    def test_parse_grbl_error_code_handles_verbose_text_form(self):
        """`$Errors/Verbose=1` 时 Grbl 回文本而非数字，解析必须两种形态都认。

        `Grbl_Esp32/src/Report.cpp:236` 在该设置为真时把 `error:11` 换成
        `error: Line too long`。只认数字的话不是降级而是行为反转：`error:8`
        （换纸期运动行被推迟、需重发）会解析成 -1 → WaitResult::Failed → 整单失败；
        `error:110`（未授权）同样落进 Failed 而非「已知未授权」。该设置存在 NVS 里，
        能被任何上位机（奎享本体、ESP3D WebUI）改写并持久化。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <string>

            int main() {
                using hutuji::ParseGrblErrorCode;

                // 数字形态（$Errors/Verbose=0，出厂默认 Defaults.h:82）
                assert(ParseGrblErrorCode("error:8") == 8);
                assert(ParseGrblErrorCode("error:11") == 11);
                assert(ParseGrblErrorCode("error:110") == 110);
                assert(ParseGrblErrorCode("error 8") == 8);   // 旧式空格分隔

                // 文本形态（$Errors/Verbose=1）——本次修复的核心。
                // 字面量逐字取自 Grbl_Esp32/src/Error.cpp 的 ErrorNames。
                assert(ParseGrblErrorCode("error: Command requires idle state") == 8);
                assert(ParseGrblErrorCode("error: Authentication failed!") == 110);
                assert(ParseGrblErrorCode("error: Line too long") == 11);
                assert(ParseGrblErrorCode("error: GCode cannot be executed in lock or alarm state") == 9);
                assert(ParseGrblErrorCode("error: Soft limit error") == 10);

                // 数字形态必须整段合法且可装入 int；畸形网络输入不得触发
                // signed overflow，也不得把带垃圾尾缀的 8 误判成 Deferred 重试。
                assert(ParseGrblErrorCode("error:8garbage") == -1);
                assert(ParseGrblErrorCode("error:8  ") == -1);
                assert(ParseGrblErrorCode("error:2147483647") == 2147483647);
                assert(ParseGrblErrorCode("error:2147483648") == -1);
                assert(ParseGrblErrorCode("error:999999999999999999999999999999") == -1);
                // 非 error 行与未收录文本仍回 -1（与旧行为一致，按 Failed 处理）
                assert(ParseGrblErrorCode("ok") == -1);
                assert(ParseGrblErrorCode("<Idle|MPos:0.000,0.000,0.000>") == -1);
                assert(ParseGrblErrorCode("error: Some future message") == -1);
                assert(ParseGrblErrorCode("error:") == -1);
                assert(ParseGrblErrorCode("") == -1);
                // 前缀相同但不是 error 行，不得误判
                assert(ParseGrblErrorCode("errors:8") == -1);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_parse_error_code_test")

    def test_auth_probe_hits_grbl_license_gate_and_crc_header_is_strict(self):
        """授权探针必须命中 Grbl 行首 G0–G3 前置门；CRC 头必须完整合法。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstdint>
            #include <string>

            int main() {
                using hutuji::BuildLicenseProbeLine;
                using hutuji::Crc32Matches;
                using hutuji::ParseCrc32Header;

                // Protocol.cpp 的授权前置门只认行首 G0~G3。G53 放前面会绕过前置门，
                // 未授权运动在 GCode.cpp 内被静默跳过但整行仍回 ok，S3 会误判已授权。
                const std::string probe = BuildLicenseProbeLine(12.5f);
                assert(probe == "G1 G53 X12.500 F1500");
                assert(probe.rfind("G1", 0) == 0);

                uint32_t crc = 0;
                assert(ParseCrc32Header("00000000", crc) && crc == 0u);
                assert(ParseCrc32Header("89abcdef", crc) && crc == 0x89abcdefu);
                assert(ParseCrc32Header("DEADBEEF", crc) && crc == 0xdeadbeefu);
                assert(Crc32Matches(crc, 0xdeadbeefu));
                assert(!Crc32Matches(crc, 0xdeadbeeeu));

                // 服务端固定输出 8 位十六进制；缺位、超长、尾缀、前缀和空白均拒。
                assert(!ParseCrc32Header("", crc));
                assert(!ParseCrc32Header("deadbee", crc));
                assert(!ParseCrc32Header("deadbeef0", crc));
                assert(!ParseCrc32Header("deadbeefjunk", crc));
                assert(!ParseCrc32Header("0xdeadbeef", crc));
                assert(!ParseCrc32Header(" deadbeef", crc));
                assert(!ParseCrc32Header("deadbeef ", crc));
                assert(!ParseCrc32Header("deadbegf", crc));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_auth_crc_contract_test")

    def test_draw_url_rejects_public_http_and_authority_confusion(self):
        """公网能力只走 HTTPS；authority 不能把允许主机藏进 userinfo。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <string>

            int main() {
                using hutuji::IsValidDrawUrl;

                assert(IsValidDrawUrl("https://hutuji.donglicao.com/files/draw_token.gcode"));
                assert(IsValidDrawUrl("https://HUTUJI.DONGLICAO.COM:443/files/draw_token.gcode?sig=x"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com:444/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("http://hutuji.donglicao.com/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com:443@evil.example/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com/files/draw_token.gcode\nInjected: x"));
                assert(!IsValidDrawUrl("https://evil.example@hutuji.donglicao.com/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com:abc/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com:0/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com:65536/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com/other"));
                assert(!IsValidDrawUrl("https://hutuji.donglicao.com/files/draw_token.gcode#fragment"));

                // 研发联调可直连 RFC1918 的同形 /files 能力；公网地址不能借此放行。
                assert(IsValidDrawUrl("http://192.168.1.2:8300/files/draw_token.gcode"));
                assert(IsValidDrawUrl("https://10.0.0.8/files/draw_token.gcode"));
                assert(IsValidDrawUrl("http://172.16.0.1/files/draw_token.gcode"));
                assert(IsValidDrawUrl("http://172.31.255.254/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("http://172.32.0.1/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("http://127.0.0.1/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("https://8.8.8.8/files/draw_token.gcode"));
                assert(!IsValidDrawUrl("ftp://hutuji.donglicao.com/files/draw_token.gcode"));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_draw_url_contract_test")

    def test_response_queue_covers_maximum_window_line_count(self):
        """最短合法非空行也不得把应答队列灌穿并丢失 error。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>

            int main() {
                using hutuji::kResponseQueueDepth;
                using hutuji::kStreamWindowBytes;

                // StreamToGrbl 用 payload+LF 计窗且要求 sum+need < window。
                // 非空 payload 最短 1B，故最多 (window-1)/2 条同时在途。
                constexpr size_t max_inflight_lines = (kStreamWindowBytes - 1u) / 2u;
                static_assert(kResponseQueueDepth >= max_inflight_lines,
                              "response queue can drop an in-flight error");
                assert(kResponseQueueDepth >= max_inflight_lines);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_response_capacity_test")


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
                using hutuji::AbortResetOwner;
                using hutuji::AbortResetOwnerPhase;
                using hutuji::AbortResetToken;
                using hutuji::CanResetAfterStream;
                using hutuji::CanSendAbortReset;
                using hutuji::IsResetSessionReady;
                using hutuji::FinishStream;
                using hutuji::IsStoppedForReset;
                using hutuji::AdvanceSendProgress;
                using hutuji::ShouldRetrySend;
                using hutuji::kSendStallBudgetMs;
                using hutuji::SendStallBudget;
                using hutuji::ShouldYieldToFeedHold;
                using hutuji::StreamQuiescence;
                using hutuji::DecideStreamSend;
                using hutuji::NextStreamControlEpoch;
                using hutuji::StreamSendCancel;

                size_t sent = 0;

                assert(!AdvanceSendProgress(sent, -1));
                assert(sent == 0);
                assert(AdvanceSendProgress(sent, 3));
                assert(sent == 3);

                assert(kSendStallBudgetMs == 20000);
                // 停滞预算只累计实际发送阶段；达到预算当刻必须过期。
                SendStallBudget active_budget(100, 20);
                assert(!active_budget.Expired(119));
                assert(active_budget.Expired(120));

                // feed hold 仲裁挂起时间不消耗发送预算，恢复后继续累计剩余活动时间。
                SendStallBudget suspended_budget(100, 20);
                suspended_budget.Suspend(105, 1005);
                assert(!suspended_budget.Expired(1019));
                assert(suspended_budget.Expired(1020));

                // TickType_t 32-bit 回绕仍按无符号差值计算。
                SendStallBudget wrapping_budget(UINT32_MAX - 5, 20);
                assert(!wrapping_budget.Expired(3));
                assert(wrapping_budget.Expired(14));

                // 部分成功只推进发送游标，不能重置最初的活动预算。
                SendStallBudget partial_send_budget(100, 20);
                assert(AdvanceSendProgress(sent, 2));
                assert(!partial_send_budget.Expired(119));
                assert(AdvanceSendProgress(sent, 1));
                assert(partial_send_budget.Expired(120));
                assert(ShouldYieldToFeedHold(false, 1));
                assert(ShouldYieldToFeedHold(false, 2));
                assert(!ShouldYieldToFeedHold(false, 0));
                assert(!ShouldYieldToFeedHold(true, 1));

                // ShouldRetrySend 只判 send() 结果和 errno，时间状态由 SendStallBudget 独立负责。
                assert(!ShouldRetrySend(1, 0));
                assert(!ShouldRetrySend(100, 0));

                // n < 0 + EAGAIN/EWOULDBLOCK 属于可重试背压。
                assert(ShouldRetrySend(-1, EAGAIN));
                assert(ShouldRetrySend(-1, EWOULDBLOCK));

                // 真实错误必须 teardown。
                assert(!ShouldRetrySend(-1, ENOTCONN));
                assert(!ShouldRetrySend(-1, EPIPE));
                assert(!ShouldRetrySend(-1, ECONNRESET));
                assert(!ShouldRetrySend(-1, 0));
                // n == 0 即使 errno 残留 EAGAIN 也必须 teardown，不能无进展循环。
                assert(!ShouldRetrySend(0, 0));
                assert(!ShouldRetrySend(0, EAGAIN));

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

                AbortResetOwner owner;
                assert(owner.Phase() == AbortResetOwnerPhase::Idle);
                assert(!owner.Started());
                assert(owner.TryClaim());
                assert(owner.Running());
                assert(!owner.TryClaim());
                assert(!owner.ResetIfSettled());
                assert(owner.Complete(true));
                assert(owner.Succeeded());
                assert(owner.ResetIfSettled());
                assert(owner.Phase() == AbortResetOwnerPhase::Idle);

                assert(owner.TryClaim());
                assert(owner.Complete(false));
                assert(owner.Phase() == AbortResetOwnerPhase::Failed);
                assert(owner.ResetIfSettled());
                assert(owner.TryClaim());
                assert(owner.CancelClaim());
                assert(owner.TryClaim());
                assert(owner.FailIfRunning());
                assert(owner.Phase() == AbortResetOwnerPhase::Failed);
                assert(owner.ResetIfSettled());
                assert(owner.Phase() == AbortResetOwnerPhase::Idle);

                assert(IsResetSessionReady(true, true, false, false));
                assert(!IsResetSessionReady(true, false, true, false));
                assert(IsResetSessionReady(true, false, true, true));
                assert(!IsResetSessionReady(false, true, true, true));

                assert(CanResetAfterStream(StreamQuiescence::Idle));
                assert(!CanResetAfterStream(StreamQuiescence::Active));
                assert(CanResetAfterStream(StreamQuiescence::Quiesced));
                assert(!CanResetAfterStream(StreamQuiescence::Failed));
                assert(FinishStream(true) == StreamQuiescence::Quiesced);
                assert(FinishStream(false) == StreamQuiescence::Failed);

                // DecideStreamSend：状态干净且纪元未动 → Allowed。
                assert(DecideStreamSend(false, false, 7, 7) == StreamSendCancel::Allowed);
                // 快照已见 paused（pause 已先在锁内提交，快照看到新纪元也匹配）→ Paused。
                assert(DecideStreamSend(false, true, 7, 7) == StreamSendCancel::Paused);
                // 快照已见 abort → Aborted；abort 优先级高于 paused。
                assert(DecideStreamSend(true, false, 7, 7) == StreamSendCancel::Aborted);
                assert(DecideStreamSend(true, true, 7, 7) == StreamSendCancel::Aborted);
                // 快照后纪元前进（pause/abort 在解锁后提交）→ 拒发，即使状态位看着干净。
                assert(DecideStreamSend(false, false, 7, 8) == StreamSendCancel::Aborted);
                assert(DecideStreamSend(false, true, 7, 8) == StreamSendCancel::Aborted);
                assert(DecideStreamSend(true, false, 7, 8) == StreamSendCancel::Aborted);
                // 纪元回绕 UINT32_MAX→0 仍判为前进（ABA 需 2^32 次提交，实际不可达）。
                assert(NextStreamControlEpoch(UINT32_MAX) == 0);
                assert(NextStreamControlEpoch(0) == 1);
                // 模拟 RequestResume：成功不动纪元；`~` 失败回滚 paused 时推进一次。
                const uint32_t before_resume = 23;
                const uint32_t resume_success_epoch = before_resume;
                assert(resume_success_epoch == before_resume);
                const uint32_t resume_rollback_epoch =
                    NextStreamControlEpoch(before_resume);
                assert(resume_rollback_epoch == 24);
                assert(DecideStreamSend(false, false, before_resume, resume_rollback_epoch) ==
                       StreamSendCancel::Aborted);
                assert(DecideStreamSend(false, true, resume_rollback_epoch,
                                        resume_rollback_epoch) == StreamSendCancel::Paused);
                assert(DecideStreamSend(false, false, UINT32_MAX, 0) ==
                       StreamSendCancel::Aborted);
                assert(DecideStreamSend(false, true, UINT32_MAX, 0) ==
                       StreamSendCancel::Aborted);
                assert(DecideStreamSend(false, false, 0, 0) == StreamSendCancel::Allowed);

                AbortResetToken token;
                assert(!token.Pending());
                assert(token.Arm(7, 20, 100));
                assert(token.Pending());
                assert(!token.Arm(7, 20, 100));
                // 发送前已收到但延迟分派的 banner 不得兑现或销毁 token。
                assert(!token.Consume(7, 21, 100));
                assert(token.Pending());
                assert(token.Consume(7, 21, 101));
                assert(!token.Pending());
                assert(!token.Consume(7, 21, 102));

                // 错 banner/session 不破坏当前 token，精确下一代仍可兑现。
                assert(token.Arm(8, 30, 200));
                assert(!token.Consume(8, 30, 201));
                assert(token.Pending());
                assert(!token.Consume(9, 31, 201));
                assert(token.Pending());
                assert(token.Consume(8, 31, 201));

                // UINT32_MAX→0 必须允许，并要求 banner 与接收 epoch 都恰好向后。
                assert(AbortResetToken::NextGeneration(UINT32_MAX) == 0);
                assert(AbortResetToken::IsAfter(0, UINT32_MAX));
                assert(!AbortResetToken::IsAfter(UINT32_MAX, 0));
                assert(token.Arm(9, UINT32_MAX, UINT32_MAX));
                assert(token.Consume(9, 0, 0));

                // Cancel 后同 session/同代禁止重新 Arm；旧 banner 先到后才可重试。
                assert(token.Arm(10, 50, 300));
                token.Cancel();
                assert(!token.Pending());
                assert(!token.Arm(10, 50, 301));
                assert(!token.Consume(10, 51, 301));
                assert(token.Arm(10, 51, 301));
                assert(token.Consume(10, 52, 302));
                return 0;
            }
            """
        )

        self._compile_and_run(compiler, source, stem="hutuji_recovery_core_test")


if __name__ == "__main__":
    unittest.main()
