import os
import re
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

            env = os.environ.copy()
            # Scoop/MinGW 的 g++.exe 可直接运行，但新编出的 exe 依赖同目录
            # libstdc++/libgcc DLL；临时目录启动时 Windows 不会自动搜索编译器目录。
            # 把该目录仅注入测试子进程 PATH，不污染全局环境或产品构建。
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

    def test_recv_wait_tick_freezes_ok_clock_during_pause(self):
        """R10-S3-01：暂停期收分支的等 ok 计时必须冻结，且暂停总时长仍受上限约束。

        Hold 期 Grbl 主循环阻塞在 sys.suspend 自旋（Protocol.cpp 的挂起循环），
        在途行躺在 RX 缓冲不被解析、ok 不会到来。若计时不冻结，
        kMotionOkTimeoutMs=30s 一到任务按「等 ok 超时」死掉，既不发 `~` 也不
        reset，写字机永久卡 Hold（只能断电解救）。冻结也不能无限：暂停累计达
        kMaxPauseMs 后必须走与 WaitWhilePaused 相同的 abort-reset 收敛。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstdint>

            int main() {
                using hutuji::DecideRecvWaitTick;
                using hutuji::RecvWaitTick;
                constexpr uint32_t kMax = 10u * 60u * 1000u;

                // 未暂停：正常计入等 ok 时钟，与暂停累计值无关。
                assert(DecideRecvWaitTick(false, 0, kMax) == RecvWaitTick::Accrue);
                assert(DecideRecvWaitTick(false, kMax, kMax) == RecvWaitTick::Accrue);

                // 暂停中且未到上限：冻结（既不计入 ok 时钟，也不判失败）。
                assert(DecideRecvWaitTick(true, 0, kMax) == RecvWaitTick::FreezePaused);
                assert(DecideRecvWaitTick(true, kMax - 1, kMax) ==
                       RecvWaitTick::FreezePaused);

                // 暂停累计达上限：走暂停超时收敛（与 WaitWhilePaused 同一条上限）。
                assert(DecideRecvWaitTick(true, kMax, kMax) == RecvWaitTick::PauseTimedOut);
                assert(DecideRecvWaitTick(true, kMax + 1, kMax) ==
                       RecvWaitTick::PauseTimedOut);
                return 0;
            }
            """
        )

        self._compile_and_run(compiler, source, stem="hutuji_recv_wait_tick_test")

    def test_auth_probe_failure_gets_bounded_retry(self):
        """R10-PIPE-01：授权探测可重试失败必须有界重试，不得一击进 Failed 终态。

        裸连接遇残留 Hold 时 `$I` 撞 Grbl 的 idleOrAlarm 门回 error:8，旧逻辑
        直接置 Failed 且唯一清除点是连接重建；而 Hold 期 `?` 有应答、silent-poll
        不判死、keepalive 不触发——连接活着就永不 ready，draw/repeat 恒报未就绪。
        修复口径：重试次数未耗尽 → 延迟后重探（RetryLater）；耗尽 → FailClosed。
        重试到期判定必须容忍 tick 回绕（uint32 半区间比较）。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstdint>

            int main() {
                using hutuji::AuthProbeFailure;
                using hutuji::AuthProbeRetryDue;
                using hutuji::DecideAuthProbeFailure;

                // 次数未耗尽 → 重试；耗尽 → fail closed（等连接重建）。
                assert(DecideAuthProbeFailure(0, 8) == AuthProbeFailure::RetryLater);
                assert(DecideAuthProbeFailure(7, 8) == AuthProbeFailure::RetryLater);
                assert(DecideAuthProbeFailure(8, 8) == AuthProbeFailure::FailClosed);
                assert(DecideAuthProbeFailure(9, 8) == AuthProbeFailure::FailClosed);
                // 上限为 0 = 从不重试（保守配置也必须成立）。
                assert(DecideAuthProbeFailure(0, 0) == AuthProbeFailure::FailClosed);

                // 到期判定：now >= due 即到期，且必须容忍 tick 回绕。
                assert(AuthProbeRetryDue(100, 100));
                assert(AuthProbeRetryDue(101, 100));
                assert(!AuthProbeRetryDue(99, 100));
                // 回绕：due 在 UINT32_MAX 附近、now 已回到小值。
                assert(AuthProbeRetryDue(5, 4294967290u));
                assert(!AuthProbeRetryDue(4294967290u, 5));
                return 0;
            }
            """
        )

        self._compile_and_run(compiler, source, stem="hutuji_auth_probe_retry_test")

    def test_gcode_command_prefix_requires_word_boundary(self):
        """S3-P3d：换纸行匹配必须按命令字边界，`M30` 不得命中 `M300`。

        `LooksLikePaperLine` 决定该行走换纸逐行模式（90s 预算 + error:8 重发）。
        裸前缀匹配下 `M300`/`M301` 等也会被当成换纸行——当前校验器禁 M 码入文件，
        不可达，但属于校验器口径漂移时的防御面，按词边界收紧。
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
                using hutuji::HasGcodeCommandPrefix;

                // 命中：整词、带参数、带空格。
                assert(HasGcodeCommandPrefix("M30", "M30"));
                assert(HasGcodeCommandPrefix("M30 P1", "M30"));
                assert(HasGcodeCommandPrefix("M721", "M721"));
                assert(HasGcodeCommandPrefix("M701 S3", "M701"));

                // 不命中：后随数字是另一条命令。
                assert(!HasGcodeCommandPrefix("M300", "M30"));
                assert(!HasGcodeCommandPrefix("M301 P1", "M30"));
                assert(!HasGcodeCommandPrefix("M7011", "M701"));

                // 不命中：不同前缀 / 空行 / 比命令字还短。
                assert(!HasGcodeCommandPrefix("G30", "M30"));
                assert(!HasGcodeCommandPrefix("", "M30"));
                assert(!HasGcodeCommandPrefix("M3", "M30"));
                return 0;
            }
            """
        )

        self._compile_and_run(compiler, source, stem="hutuji_paper_prefix_test")

    def test_page_end_uses_non_motion_m30_orchestration(self):
        """页尾换纸必须单发 M30，不得把回原点运动塞进不可即停的换纸窗口。

        归位（2026-08-14 用户决策）由 `ReturnHomeAfterDraw()` 在正常页尾以
        独立 G1 行先行完成，不进本函数的换纸窗口——本断言继续钉死 M30 是
        换纸窗口内唯一命令。
        """
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        start = source.index("bool Job::ChangePaperAfterDraw()")
        end = source.index("\n}\n\nstd::vector<Job::LineSpan>", start)
        body = source[start:end]

        commands = re.findall(r'pipe\.SendLine\("([^"]+)"\)', body)
        self.assertEqual(commands, ["M30"])

    def test_return_home_is_g1_and_only_on_normal_page_end(self):
        """页尾归位（2026-08-14 用户决策）必须是 G1 且只在正常页尾路径。

        固件「回原点后换纸」触发只认本行实际执行的 G0/G28/G30 且落点 XY≈0
        （GCode.cpp 页尾分支，`block_executed_seek` 仅在 Motion::Seek 分支
        置位）；G1 落 (0,0) 不触发。归位行若误用 G0，归位与换纸会耦进同一条
        90s 不可即停的换纸行，违反上个测试钉死的口径。断连恢复的废纸换纸
        （RecoverDisconnectedDraw 内 ChangePaperAfterDraw 调用点）不归位：
        该路径刚经历受限 reset，position 可信度最低，只允许固定恢复序列。
        """
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")

        # 归位实现：发送的必须是 G1 形式的原点行，且函数内只有这一条发送。
        start = source.index("bool Job::ReturnHomeAfterDraw()")
        end = source.index("bool Job::ChangePaperAfterDraw()", start)
        body = source[start:end]
        homes = re.findall(r'pipe\.SendLine\("([^"]+)"\)', body)
        self.assertEqual(homes, ["G1G90 X0Y0F8000"])
        # 归位必须「ok 后再 fresh Idle」：G1 的 ok 只表示入 planner，缺了
        # WaitForIdle 就退化成靠 M30 内部 synchronize 的隐性顺序保证，abort 在
        # 归位/换纸两阶段之间没有真实决策点。
        ok_wait = body.index("pipe.WaitResponse(")
        idle_wait = body.index("WaitForIdle(true, kHomeIdleTimeoutMs)")
        self.assertLess(ok_wait, idle_wait)
        self.assertTrue(homes[0].startswith("G1"), homes)

        # 正常页尾路径：先归位、后换纸（同行序调用）。
        park_call = source.index("ok = ReturnHomeAfterDraw();")
        change_call = source.index("ok = ChangePaperAfterDraw();", park_call)
        self.assertLess(park_call, change_call)

        # 恢复路径：不得含归位（函数体以 ReturnHomeAfterDraw 定义为界）。
        rec_start = source.index("bool Job::RecoverDisconnectedDraw()")
        rec_body = source[rec_start:start]
        self.assertNotIn("ReturnHomeAfterDraw", rec_body)
        self.assertNotIn("G1G90 X0Y0", rec_body)

    def test_describe_grbl_error_mapping(self):
        """R20-S3-04：用户面中文映射表——8/10/110 有描述；90（通用 MessageFailed）
        与未知码必须回 nullptr（调用方保持原文），不得把通用码说成缺纸。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstring>

            int main() {
                using hutuji::DescribeGrblError;
                assert(std::strstr(DescribeGrblError(8), "暂不能执行"));
                assert(std::strstr(DescribeGrblError(10), "软限位"));
                assert(std::strstr(DescribeGrblError(110), "未授权"));
                assert(DescribeGrblError(90) == nullptr);   // 通用码不映射
                assert(DescribeGrblError(999) == nullptr);  // 未知码
                assert(DescribeGrblError(-1) == nullptr);
                return 0;
            }
            """
        )

        self._compile_and_run(compiler, source, stem="hutuji_describe_error_test")

    def test_pause_timeout_notify_follows_reset_outcome(self):
        """R20-S3-03：暂停超时的取消话术必须跟随 reset owner 创建结果。

        StartAbortResetTask 成功只代表 worker 已创建（0x18 在异步 worker 里仍可能
        失败），故成功分支只许说「已启动自动取消」；「已自动取消」的完成式文案
        与「先 Notify 后启动」的旧顺序都必须不存在。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        start = source.index("void Job::CommitPauseTimeoutCancel()")
        body = source[start : source.index("\n}\n", start)]

        self.assertNotIn("已自动取消这幅画", body)
        self.assertIn("已启动自动取消", body)
        self.assertIn("自动取消失败", body)
        self.assertLess(body.index("StartAbortResetTask()"), body.index("Notify("))

    def test_recv_loop_rechecks_abort_hold_before_ok_fallback(self):
        """R11-PIPE-01：等待循环退出后必须再复核一次「abort 已提交且 Hold 已确认」。

        暂停超时提交 abort 时，在途行的 `waited` 往往只差最后一个 slice：
        `CommitPauseTimeoutCancel` 后 continue，下一片 TakeResponse 超时返回即让
        `waited` 走满 timeout，`while (waited < timeout)` 先判假 —— 循环顶部那道
        「排流 + 清窗 + MarkQuiesced」分支再也执行不到。落到 ok 兜底则必然失败
        （`ConfirmInFlightDoneByStatus` 要 fresh Idle，机器在 Hold），而该失败路径
        不 MarkQuiesced → `FinishStream(false)` = Failed → abort owner 的
        `CanResetAfterStream(Failed)` 为假 → 受限 reset 发不出去 → 写字机滞留
        Hold（只能外部 `~`/断电）且连接被拆。故复核点必须落在「等待片累计」与
        「ok 兜底」之间。
        """
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        start = source.index("bool Job::StreamToGrbl()")
        body = source[start:]

        accrue = body.rindex("waited += step;")
        fallback = body.index("ok_fallback_count < kMaxOkFallback")
        self.assertLess(accrue, fallback)
        between = body[accrue:fallback]

        self.assertIn("abort_hold_confirmed_", between)
        # 仅复核不够：命中后必须走与循环顶部同款的收敛（含 MarkQuiesced），
        # 否则 quiescence 仍是 Failed，受限 reset 照样被自家门拒。
        self.assertIn("quiesce_for_abort()", between)

    def test_status_position_requires_finite_fresh_mpos(self):
        """P1：NaN/Inf 与只含 WPos 的 fresh status 都不能成为丢 ok 的位置证据。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cmath>

            int main() {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                assert(hutuji::ParseFiniteMPos("Idle|MPos:1.25,2.5,0", x, y, z));
                assert(std::fabs(x - 1.25f) < 0.001f);
                assert(!hutuji::ParseFiniteMPos("Idle|MPos:1.25,2.5,0garbage", x, y, z));
                assert(!hutuji::ParseFiniteMPos("Idle|MPos:nan,2.5,0", x, y, z));
                assert(!hutuji::ParseFiniteMPos("Idle|MPos:inf,2.5,0", x, y, z));
                assert(!hutuji::ParseFiniteMPos("Idle|WPos:1.25,2.5,0", x, y, z));
                assert(!hutuji::ParseFiniteMPos("Idle|MPos:1.25,2.5", x, y, z));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_finite_mpos_test")

        pipe_h = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.h").read_text(encoding="utf-8")
        pipe_cc = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(encoding="utf-8")
        job_cc = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        self.assertIn("GetMposReportSequence", pipe_h)
        self.assertIn("mpos_report_seq_.fetch_add(1)", pipe_cc)
        fallback_start = job_cc.index("bool Job::ConfirmInFlightDoneByStatus")
        fallback_end = job_cc.index("bool Job::WaitForIdle", fallback_start)
        fallback = job_cc[fallback_start:fallback_end]
        self.assertIn("GetMposReportSequence", fallback)
        self.assertIn("mpos_seq", fallback)

    def test_open_hotspot_wifi_qr_payload_escapes_ssid(self):
        """二维码 payload 按 ZXing 规则转义，LCD 共享覆盖层覆盖两块板。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"
            #include <cassert>
            #include <string>
            int main() {
                assert(hutuji::BuildOpenHotspotWifiQrPayload("Xiaozhi-ABCD") ==
                       "WIFI:T:nopass;S:Xiaozhi-ABCD;;");
                assert(hutuji::BuildOpenHotspotWifiQrPayload("My;Wifi:Name\\x,\"y\"") ==
                       "WIFI:T:nopass;S:My\\;Wifi\\:Name\\\\x\\,\\\"y\\\";;");
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_hotspot_qr_test")

        component = ROOT / "components/hutuji_qrcode/include/qrcode.h"
        display_h = (ROOT / "main/display/display.h").read_text(encoding="utf-8")
        lcd_h = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        lichuang = (ROOT / "main/boards/lichuang-dev/lichuang_dev_board.cc").read_text(
            encoding="utf-8"
        )
        waveshare = (ROOT / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc").read_text(
            encoding="utf-8"
        )
        self.assertTrue(component.is_file())
        self.assertIn("virtual void ShowProvisioningQr", display_h)
        self.assertIn("void ShowProvisioningQr", lcd_h)
        self.assertIn("esp_qrcode_generate", lcd_cc)
        self.assertIn("LV_OBJ_FLAG_CLICKABLE", lcd_cc)
        self.assertIn("LV_OBJ_FLAG_SCROLLABLE", lcd_cc)
        for board, constructor in ((lichuang, "LichuangDevBoard"), (waveshare, "CustomBoard")):
            self.assertIn("BuildOpenHotspotWifiQrPayload", board)
            self.assertIn("void SetNetworkEventCallback(NetworkEventCallback callback) override", board)
            self.assertIn("WifiBoard::SetNetworkEventCallback(", board)
            self.assertIn("callback = std::move(callback)", board)
            callback_start = board.index("void SetNetworkEventCallback(NetworkEventCallback callback) override")
            callback_end = board.index(f"\n    {constructor}(", callback_start)
            callback_body = board[callback_start:callback_end]
            self.assertIn("WifiConfigModeEnter", callback_body)
            self.assertIn("WifiConfigModeExit", callback_body)
            self.assertIn("ShowProvisioningQr", callback_body)
            self.assertIn("HideProvisioningQr", callback_body)
            self.assertIn("if (callback)", callback_body)
        self.assertNotIn("class HotspotQrDisplay", lichuang)

    def test_waveshare_uses_grobot_face_gate_and_scaled_canvas(self):
        """Waveshare 使用独立 Grobot 门控、460x300 画布，且两套 UI 都初始化。"""
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        lcd_h = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        self.assertIn("HUTUJI_GROBOT_FACE", cmake)
        self.assertIn("boards/lichuang-dev/grobot_eyes.cc", cmake)
        self.assertIn("CONFIG_HUTUJI_GROBOT_FACE", lcd_h)
        self.assertIn("CONFIG_HUTUJI_GROBOT_FACE", lcd_cc)
        self.assertIn("constexpr int kFaceWidth = 460", lcd_cc)
        self.assertIn("constexpr int kFaceHeight = 300", lcd_cc)
        self.assertIn("void LcdDisplay::InitializeEmotionUi", lcd_cc)
        self.assertEqual(lcd_cc.count("InitializeEmotionUi(screen, lvgl_theme, large_icon_font)"), 2)
    def test_waveshare_writer_keeps_power_on_after_idle(self):
        """写字机需保持联网待命：可降亮度，但不得五分钟后由 AXP2101 断电。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        self.assertIn("PowerSaveTimer(-1, 60, -1)", board)
        self.assertIn("pmic_->PowerOff()", board)
    def test_waveshare_boot_button_wakes_power_save(self):
        """省电后按 BOOT 必须先恢复背光和正常电源状态，再切换聊天。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeButtons()")
        end = board.index("    // 初始化工具", start)
        body = board[start:end]
        self.assertIn("power_save_timer_->WakeUp();", body)
        self.assertLess(body.index("power_save_timer_->WakeUp();"), body.index("app.ToggleChatState();"))

    def test_window_error_requests_immediate_hold_and_controlled_reset(self):
        """P1：窗口 error 后 RX 后续行仍会执行；必须立即 hold 并在退窗后受控 reset。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        stream_start = source.index("bool Job::StreamToGrbl()")
        stream_body = source[stream_start:]
        self.assertIn("fail_window_and_stop", stream_body)
        self.assertGreaterEqual(stream_body.count("return fail_window_and_stop("), 2)
        helper_start = stream_body.index("fail_window_and_stop")
        helper = stream_body[helper_start : stream_body.index("};", helper_start) + 2]
        self.assertIn("SendRealtime('!')", helper)
        self.assertIn("DrainResponses()", helper)
        self.assertIn("c_line.clear()", helper)
        self.assertIn("window_guard.MarkQuiesced()", helper)
        self.assertIn("stream_error_stop_required_.store(true", helper)

        run_start = source.index("void Job::Run()")
        run_end = source.index("bool Job::DownloadToPsram", run_start)
        run_body = source[run_start:run_end]
        self.assertIn("stream_error_stop_required_.exchange(false", run_body)
        self.assertIn("PerformAbortReset(false", run_body)
        reset_at = run_body.index("stream_error_stop_required_.exchange(false")
        publish_at = run_body.index('SetState("error")', reset_at)
        self.assertLess(reset_at, publish_at)

    def test_waveshare_touch_wakes_power_save(self):
        """FT5x06 首次触摸应退出低亮度省电，同时保留 LVGL 原触摸分发。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertIn("lv_indev_t* touch_indev = lvgl_port_add_touch(&touch_cfg)", body)
        self.assertIn("lv_indev_add_event_cb(", body)
        self.assertIn("touch_indev,", body)
        self.assertIn("LV_EVENT_PRESSED", body)
        self.assertIn("timer->WakeUp();", body)

    def test_waveshare_logs_live_touch_controller_registers(self):
        """触摸调参必须先读取板上真实 threshold/rate/chip/vendor，不盲写官方示例值。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertIn("kFt6x36ThresholdReg = 0x80", body)
        self.assertIn("kFt6x36ActivePeriodReg = 0x88", body)
        self.assertIn("kFt6x36ChipIdReg = 0xA3", body)
        self.assertIn("kFt6x36VendorIdReg = 0xA8", body)
        self.assertIn("esp_lcd_panel_io_rx_param", body)
        self.assertIn(
            "Touch registers: threshold=%u active_period=%u chip=0x%02X vendor=0x%02X",
            body,
        )

    def test_waveshare_applies_experimental_touch_threshold_with_readback(self):
        """实机默认 70；官方 60 仍漏触时实验调到 40，并回读验证。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertIn("kTouchThreshold = 40", body)
        self.assertIn("esp_lcd_panel_io_tx_param", body)
        self.assertIn("Touch threshold tuned: %u -> %u", body)
        self.assertIn("Touch threshold tune failed", body)
        self.assertEqual(body.count("esp_lcd_panel_io_tx_param"), 1)
        self.assertIn("kFt6x36ThresholdReg, &kTouchThreshold", body)

    def test_waveshare_touch_diagnostics_are_not_left_in_production(self):
        """逐触摸原始坐标和按下日志只用于实机定位，最终固件不得持续刷日志。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertNotIn(".process_coordinates", body)
        self.assertNotIn("Touch diagnostic:", body)
        self.assertNotIn("lv_indev_get_point", body)
        self.assertNotIn("lv_event_get_indev", body)

    def test_waveshare_touch_bounds_match_raw_axes_before_swap(self):
        """FT6X36 原始轴为 320x480；镜像先于 swap，配置边界必须按原始轴。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertIn(".x_max = DISPLAY_HEIGHT", body)
        self.assertIn(".y_max = DISPLAY_WIDTH", body)
        self.assertIn(".swap_xy = 1", body)
        self.assertNotIn("lv_indev_get_read_timer", body)
        self.assertNotIn("lv_timer_set_period", body)

    def test_paper_change_resets_blocking_peer_by_raii(self):
        """R11-PIPE-02：换纸编排的 blocking 标记必须 RAII 复位，不留手工出口。

        `ChangePaperAfterDraw` 的 M30 等待循环有 link-lost 与 ALARM 两个早退
        `return false`，手工 `SetExpectBlockingPeer(false)` 覆盖不到它们；
        `RecoverDisconnectedDraw` 的换纸窗口轮询分支更是完全不复位。两处此前都只靠
        `Run()` 收尾兜底，属隐性契约：再插一个等待分支就是真泄漏。与换纸行分支既有的
        BlockingGuard 统一成 RAII —— 全文件的复位调用都只许长在析构那一行上。
        """
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")

        for fn, next_symbol in (
            ("bool Job::ChangePaperAfterDraw()", "\n}\n\nstd::vector<Job::LineSpan>"),
            ("bool Job::RecoverDisconnectedDraw()", "\nbool Job::ReturnHomeAfterDraw()"),
        ):
            start = source.index(fn)
            body = source[start : source.index(next_symbol, start)]
            with self.subTest(fn=fn):
                self.assertIn("BlockingGuard", body)
                resets = re.findall(
                    r"^.*SetExpectBlockingPeer\(false\).*$", body, re.MULTILINE
                )
                self.assertEqual(len(resets), 1, resets)
                self.assertIn("~BlockingGuard()", resets[0])
                self.assertEqual(body.count("SetExpectBlockingPeer(true)"), 1)

        # Run() 收尾那一处是任务级兜底（不在这两个函数体内），保持原样：全文件恰好
        # 三处 RAII 析构 + 一处收尾兜底，多出任何手工复位都说明又开了新出口。
        self.assertEqual(source.count("SetExpectBlockingPeer(false)"), 4)

    def test_describe_transfer_failure_mapping(self):
        """R21-F03：下载/校验失败的用户面话术——404 引导重新生成（TTL 过期是正常
        路径），CRC/长度类报文件不完整，其余网络/资源类报稍后重试；未知串走兜底。
        技术诊断串由调用方留 ESP 日志，不进用户播报。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstring>

            int main() {
                using hutuji::DescribeTransferFailure;
                assert(std::strstr(DescribeTransferFailure("HTTP status 404"), "重新生成"));
                assert(std::strstr(DescribeTransferFailure("CRC 不符"), "不完整"));
                assert(std::strstr(DescribeTransferFailure("Content-Length 为 0"), "不完整"));
                assert(std::strstr(DescribeTransferFailure("PSRAM 分配失败"), "重试"));
                assert(std::strstr(DescribeTransferFailure("HTTP Open 失败"), "重试"));
                return 0;
            }
            """
        )

        self._compile_and_run(compiler, source, stem="hutuji_transfer_failure_test")

    def test_abort_completion_notify_after_reset_outcome(self):
        """R21-F02：abort 终态播报必须晚于 reset 结果判定，且在 stream_mutex_ 外发出。

        do-while 内的 aborted 分支早于 reset 判定（WaitForAbortReset/owner 结果在
        收尾 while 里才确定），那里说「已停止」在 reset 失败时是假话；而收尾循环
        持有 stream_mutex_（新任务发布门），锁内 Notify 会堵发布。故话术只能长在
        收尾 while 之后：钉死「已停止/取消失败」Notify 出现在 reset 判定之后。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        start = source.index("void Job::Run()")
        body = source[start : source.index("\n}\n", start)]

        decided = body.index('"abort reset 恢复失败"')
        stopped = body.index('Notify("已停止")')
        failed = body.index("取消失败，写字机可能未停稳")
        self.assertLess(decided, stopped)
        self.assertLess(decided, failed)
        # 两个播报都必须在收尾 while 的 break 之后（循环外、锁外）。
        tail_break = body.rindex("break;")
        self.assertLess(tail_break, stopped)
        self.assertLess(tail_break, failed)

    def test_paper_change_notify_is_once_gated(self):
        """R21-F01：换纸播报两个入口（文件内换纸行 / 页尾与恢复的 ChangePaperAfterDraw）
        共用 paper_change_notified_ 门控，整张任务只播一次；门控随 Run() 复位。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")

        run_start = source.index("void Job::Run()")
        run_body = source[run_start : source.index("\n}\n", run_start)]
        self.assertIn("paper_change_notified_ = false;", run_body)

        announces = source.count('Notify("正在换纸，请稍候")')
        self.assertEqual(announces, 2)  # 恰好两个入口，文案一致
        gates = source.count("if (!paper_change_notified_)")
        self.assertEqual(gates, 2)  # 两个入口都在门控内
        self.assertEqual(source.count("paper_change_notified_ = true;"), 2)

    def test_pipe_transition_notify_wording(self):
        """R21-F04：状态转移播报去英文状态名/机器坐标/内网 IP；Run→Idle 与 Hold
        必须带 10s 去抖（页尾归位会确定性连触发两次 Run→Idle）。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc"
        ).read_text(encoding="utf-8")
        self.assertNotIn("写字机运动完成 (Run→Idle)", source)
        self.assertNotIn("写字机暂停 (Hold)", source)
        self.assertNotIn("ALARM:%d 位置", source)
        self.assertNotIn('resolved_ip_ + ")"', source)
        self.assertIn('NotifyCloud("写字机运动完成")', source)
        self.assertIn('NotifyCloud("写字机已暂停")', source)
        self.assertIn('NotifyCloud("写字机已连接")', source)
        # 去抖：两个分支共用同一纪元。
        self.assertEqual(source.count("last_transition_notify_tick_"), 4)  # 两分支各一查一写

    def test_abort_window_suppresses_hold_transition_notify(self):
        """R21-F04 残余：abort 受限 reset 期（PerformAbortReset 全程，RAII）抑制
        Run→Idle/Hold 转移播报——`!` 造成的 Hold 是「取消中」不是用户暂停；
        正常 pause 不走该函数，其 Hold 播报不受影响。ALARM 转移不抑制（安全）。"""
        pipe_cc = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(encoding="utf-8")
        pipe_h = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.h").read_text(encoding="utf-8")
        job_cc = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")

        # 两个转移分支都查抑制标记；ALARM 分支不查。
        self.assertEqual(pipe_cc.count("!transition_notify_suppressed_.load()"), 2)
        self.assertIn("SetTransitionNotifySuppressed", pipe_h)

        start = job_cc.index("bool Job::PerformAbortReset(")
        body = job_cc[start : job_cc.index("\n}\n", start)]
        guard = body.index("TransitionNotifyGuard")
        feed_hold = body.index("SendRealtime('!')")
        self.assertLess(guard, feed_hold)  # 抑制必先于 `!`
        self.assertIn("~TransitionNotifyGuard()", body)  # RAII 覆盖所有 break 早退

    def test_progress_notify_percent_only(self):
        """R21-F05：进度播报只留百分比；坐标/行号不再进 Notify（串口日志仍有）。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        self.assertIn('"出图进度: %d%%"', source)
        self.assertNotIn("出图进度: %d%% (%zu/%zu行)", source)

    def test_failure_notify_single_exit(self):
        """R21-F08：断连换纸窗口分支已播报后，Run() 收尾失败出口不得二次播报。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        rec_start = source.index("bool Job::RecoverDisconnectedDraw()")
        rec_body = source[rec_start : source.index("bool Job::ReturnHomeAfterDraw()", rec_start)]
        self.assertIn("failure_notified_ = true;", rec_body)
        run_start = source.index("void Job::Run()")
        run_body = source[run_start : source.index("\n}\n", run_start)]
        self.assertIn("failure_notified_ = false;", run_body)
        self.assertIn("if (!failure_notified_)", run_body)

    def test_error8_exhaustion_wording(self):
        """R21-F13：error:8 重试耗尽的用户面文案不得带 error:8 原文（映射层会把
        「正在换纸」进行式拼到「已停止」结论上）；技术细节只留 ESP 日志。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        self.assertIn('last_error_ = "换纸未完成，已停止本次绘图，请检查纸张后重试"', source)
        # 旧文案只允许以日志形态存在。
        self.assertNotIn('last_error_ = "error:8 重试耗尽"', source)
        self.assertIn('ESP_LOGE(TAG, "换纸行 error:8 重试耗尽', source)

    def test_error90_paper_source_sentence(self):
        """R21-F07：换纸路径 err==90 在源头给「请整理纸张」动作句；通用码映射契约
        保持 90==nullptr（test_describe_grbl_error_mapping 钉死）不变。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        start = source.index("bool Job::ChangePaperAfterDraw()")
        body = source[start : source.index("\n}\n\nstd::vector<Job::LineSpan>", start)]
        self.assertIn("if (err == 90)", body)
        self.assertIn("纸张用完或未放好，请整理好纸张后再试", body)

    def test_busy_done_repeat_wording(self):
        """R21-F14：busy 裸英文 token 不得存在；repeat 有起始播报；done 话术带
        「再来一次」引导（出图成功 buffer 留存，重画真实可用）。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        self.assertNotIn('JsonString("busy")', source)
        self.assertEqual(source.count('JsonString("写字机正忙，请稍候再试")'), 3)
        self.assertIn('Notify("出图完成，可以说「再来一次」直接重画")', source)
        self.assertIn('Notify("开始重画")', source)

    def test_reconnect_wait_has_heartbeat(self):
        """R21-F06/F12：断连恢复重连等待（最长 120s）必须有 30s 进展播报；
        超时话术附可执行动作。"""
        source = (
            ROOT / "main/boards/lichuang-dev/hutuji_job.cc"
        ).read_text(encoding="utf-8")
        rec_start = source.index("bool Job::RecoverDisconnectedDraw()")
        rec_body = source[rec_start : source.index("bool Job::ReturnHomeAfterDraw()", rec_start)]
        self.assertIn('Notify("还在重连写字机，请稍候")', rec_body)
        self.assertIn("pdMS_TO_TICKS(30000)", rec_body)
        self.assertIn("等待写字机重连超时，请检查写字机电源和网络后重新开始", rec_body)

    def test_grobot_eyes_gaze_wink_color_and_state_hooks(self):
        """眼睛细化包（参考 FluxGarage/RoboEyes 语义）：MoodData 带 lookX/lookY 视线；
        winking 必须不对称（右眼全闭）；情绪配色表存在；说话/聆听钩子接入
        LcdDisplay::SetStatus 且调用基类保住状态栏文本。"""
        eyes_h = (ROOT / "main/boards/lichuang-dev/grobot_eyes.h").read_text(
            encoding="utf-8"
        )
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        # 视线维度进情绪模型与弹簧插值
        self.assertIn("float lookX;", eyes_h)
        self.assertIn("float lookY;", eyes_h)
        self.assertIn("SetSpeaking(bool on)", eyes_h)
        self.assertIn("SetListening(bool on)", eyes_h)
        self.assertIn("ApplySpring(c.lookX, c.vLookX, t.lookX, dt_, 120, 18)", eyes_cc)
        # thinking 看右上（RoboEyes setPosition 语义）
        self.assertIn("{10, 15, -20, 28, 43, 0.45f, -0.40f}", eyes_cc)
        # 高光跟随瞳孔而非固定眼心
        self.assertIn("pcx - eyeR / 4, pcy - eyeR / 4", eyes_cc)

        # winking 修复：i == 12 右眼全闭（topH=100 与 Blink() 全闭量一致）
        self.assertIn("i == 12", eyes_cc)
        wink_line = next(
            line for line in eyes_cc.splitlines() if "right = {100, 0, 0, 25, 45" in line
        )
        self.assertIn("0.15f, 0", wink_line)

        # 情绪配色：angry 红 / loving 粉 / thinking 紫 必须在表内
        self.assertIn("kMoodColors", eyes_cc)
        self.assertIn("0xFF7A70", eyes_cc)
        self.assertIn("0xFF7EB6", eyes_cc)
        self.assertIn("0xBF8FFF", eyes_cc)
        self.assertIn("RecomputePalette(color != 0 ? lv_color_hex(color) : eye_color_default_)", eyes_cc)

        # 说话/聆听钩子：LcdDisplay::SetStatus 先调基类再驱动眼睛
        self.assertIn("void LcdDisplay::SetStatus(const char* status)", lcd_cc)
        status_start = lcd_cc.index("void LcdDisplay::SetStatus(const char* status)")
        status_end = lcd_cc.index("void LcdDisplay::SetEmotion", status_start)
        status_body = lcd_cc[status_start:status_end]
        self.assertIn("LvglDisplay::SetStatus(status);", status_body)
        self.assertIn("grobot_eyes_->SetSpeaking(std::strcmp(status, Lang::Strings::SPEAKING) == 0)", status_body)
        self.assertIn("grobot_eyes_->SetListening(std::strcmp(status, Lang::Strings::LISTENING) == 0)", status_body)
        # 基类调用必须在眼睛驱动之前（状态栏文本不能被截掉）
        self.assertLess(
            status_body.index("LvglDisplay::SetStatus(status);"),
            status_body.index("grobot_eyes_->SetSpeaking"),
        )
        # 说话弹跳与聆听放大落在渲染侧
        self.assertIn("speaking_", eyes_cc)
        self.assertIn("listening_ ? 1.18f : 1.0f", eyes_cc)

    def test_grobot_eyes_p2_saccade_arc_lids_heart_aa_and_palette(self):
        """P2 细化包：空闲眼跳（RoboEyes setIdleMode 语义）/ 弧线眼睑 / loving·kissy
        心形瞳 / 圆边抗锯齿（与底层像素混合）/ 配色族系重排（喜悦族暖金）。"""
        eyes_h = (ROOT / "main/boards/lichuang-dev/grobot_eyes.h").read_text(
            encoding="utf-8"
        )
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )

        # 空闲眼跳：非说话/聆听时 1.5~4s 间隔 + 独立软弹簧 + 累加进视线
        self.assertIn("saccade_interval_ms_ = 1500 + esp_random() % 2500", eyes_cc)
        self.assertIn("ApplySpring(saccCurX_, saccVelX_, saccX_, dt_, 160, 20)", eyes_cc)
        self.assertIn("(c.lookX + saccCurX_) * eyeR_px * 0.30f", eyes_cc)
        sacc_start = eyes_cc.index("// 空闲眼跳")
        sacc_body = eyes_cc[sacc_start : eyes_cc.index("ApplySpring(saccCurX_", sacc_start)]
        self.assertIn("!speaking_ && !listening_", sacc_body)

        # 弧线眼睑：逐列抛物线切割，全闭保持平切
        self.assertIn("BufLidArc", eyes_h)
        self.assertIn("lidH >= 100 ? lidH : (int)(lidH * (1.0f - 0.45f * nx * nx))", eyes_cc)
        draw_start = eyes_cc.index("void GrobotEyes::DrawEye")
        draw_body = eyes_cc[draw_start : eyes_cc.index("void GrobotEyes::Render", draw_start)]
        self.assertIn("BufLidArc(cx, cy, eyeR, pad, lidH, true)", draw_body)
        self.assertIn("BufLidArc(cx, cy, eyeR, pad, botH, false)", draw_body)
        self.assertNotIn("(eyeR + pad) * 2, lidH + pad, bg_color_);", draw_body)

        # 心形瞳：仅 loving(7)/kissy(16)
        self.assertIn("void GrobotEyes::BufFillHeart", eyes_cc)
        self.assertIn("mood_index_ == 7 || mood_index_ == 16", eyes_cc)
        self.assertIn("heartPupil", eyes_h)

        # 抗锯齿：圆边 1px 按覆盖率与底层像素混合（Blend565）
        self.assertIn("Blend565", eyes_h)
        self.assertIn("buf_[row * s + cl] = Blend565(cv, buf_[row * s + cl], frac)", eyes_cc)

        # 配色族系：喜悦族暖金进表，happy/laughing/funny/winking 四席
        self.assertEqual(eyes_cc.count("0xFFCF3F"), 4)
        self.assertIn("0xD8B4E2", eyes_cc)  # embarrassed 薰衣草
        self.assertIn("0xBDF3FF", eyes_cc)  # cool 冰蓝
        self.assertIn("0xFF6B35", eyes_cc)  # delicious 橙
        self.assertIn("0xFF9770", eyes_cc)  # silly 蜜桃



    def test_grobot_full_face_layers_and_speaking_mouth_contract(self):
        """全脸升级保持 Grobot renderer：眉毛/嘴巴/腮红泪滴汗滴火花均为程序化
        LVGL 图层；speaking 通过嘴部开合表达，且脸部画布为 280x190。"""
        eyes_h = (ROOT / "main/boards/lichuang-dev/grobot_eyes.h").read_text(
            encoding="utf-8"
        )
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        # 全脸参数由情绪 preset 驱动，并用弹簧平滑过渡
        self.assertIn("struct FacialData", eyes_h)
        self.assertIn("kFacialMoods", eyes_cc)
        self.assertIn("ApplyFacialData", eyes_cc)
        self.assertIn("ApplySpring(face_cur_.mouthOpen", eyes_cc)
        self.assertIn("ApplySpring(face_cur_.browTiltL", eyes_cc)

        # 程序化图层：不依赖 GIF/Emote 资源
        self.assertIn("BufFillEllipse", eyes_h)
        self.assertIn("BufLine", eyes_h)
        self.assertIn("DrawEyebrows", eyes_cc)
        self.assertIn("DrawMouth", eyes_cc)
        self.assertIn("DrawFacialEffects", eyes_cc)
        self.assertIn("blush", eyes_cc)
        self.assertIn("tears", eyes_cc)
        self.assertIn("sweat", eyes_cc)
        self.assertIn("sparkle", eyes_cc)
        self.assertIn("{18, 18", eyes_cc)  # angry 双眉镜像内压但限制攻击性
        self.assertIn("for (int i = 1; i <= 16; i++)", eyes_cc)  # 嘴线分段平滑

        # speaking 的可见反馈是嘴部开合，不只是眼睛 bounce
        self.assertIn("speaking_", eyes_cc)
        self.assertIn("mouthOpenTarget", eyes_cc)
        self.assertIn("DrawMouth", eyes_cc)

        # 给全脸留出纵向构图空间；Waveshare 使用更大画布，二维码显示路径不换 renderer
        self.assertIn("constexpr int kFaceWidth = 460", lcd_cc)
        self.assertIn("constexpr int kFaceHeight = 300", lcd_cc)
        self.assertNotIn("CONFIG_USE_EMOTE_MESSAGE_STYLE", lcd_cc)
        self.assertNotIn("otto_emoji_gif", eyes_cc)
        self.assertNotIn("emote::EmoteDisplay", lcd_cc)


    def test_grobot_full_face_suppresses_chat_overlay(self):
        """全脸模式不显示聊天正文：字幕栏会覆盖嘴巴/下半脸，必须在统一入口
        清空并保持隐藏；状态栏与 SetStatus 不受影响。"""
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        bodies = []
        cursor = 0
        while True:
            start = lcd_cc.find("void LcdDisplay::SetChatMessage", cursor)
            if start < 0:
                break
            end = lcd_cc.find("void LcdDisplay::ClearChatMessages", start)
            self.assertGreaterEqual(end, 0)
            bodies.append(lcd_cc[start:end])
            cursor = end
        self.assertGreaterEqual(len(bodies), 2)
        for body in bodies:
            self.assertIn("if (grobot_eyes_ != nullptr)", body)
            self.assertIn("lv_label_set_text(chat_message_label_, \"\")", body)
            self.assertIn("lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN)", body)
            self.assertIn("return;", body)
    def test_grobot_face_background_keeps_features_clear(self):
        """全脸背景保留弱扫描线但不画穿脸中线；清空聊天时 Grobot 不复活 AI logo。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        bg_start = eyes_cc.index("void GrobotEyes::DrawBackground")
        bg_end = eyes_cc.index("void GrobotEyes::DrawEye", bg_start)
        bg_body = eyes_cc[bg_start:bg_end]
        self.assertEqual(eyes_cc.count("scan_color_ ="), 1)
        self.assertIn("scan_color_ = lv_color_mix(eye_color_, bg_color_, 7)", eyes_cc)
        self.assertNotIn("BufHLine(0, w_ - 1, h_ / 2", bg_body)

        clear_start = lcd_cc.index("void LcdDisplay::ClearChatMessages")
        clear_end = lcd_cc.index("#else", clear_start)
        clear_body = lcd_cc[clear_start:clear_end]
        self.assertIn("grobot_eyes_ == nullptr", clear_body)
        self.assertIn("lv_obj_remove_flag(emoji_label_", clear_body)

    def test_grobot_neutral_face_has_nose_and_fuller_resting_lips(self):
        """neutral 不能只有一条嘴线：需要低对比度圆角机器人鼻、较宽上下唇弧，
        同时保留 speaking 张合嘴腔。"""
        eyes_h = (ROOT / "main/boards/lichuang-dev/grobot_eyes.h").read_text(
            encoding="utf-8"
        )
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("DrawNose", eyes_h)
        self.assertIn("void GrobotEyes::DrawNose", eyes_cc)
        self.assertIn("const int width = eyeR * 11 / 5", eyes_cc)
        self.assertIn("const int lipGap = std::max(6, open / 2)", eyes_cc)
        self.assertIn("const int lipSeparation = lipGap - lipGap * iabs / 8", eyes_cc)
        self.assertIn("nextUpperY", eyes_cc)
        self.assertIn("nextLowerY", eyes_cc)
        self.assertIn("{0, 0, 0, 0, 0.48f, 0.12f", eyes_cc)

    def test_grobot_sleepy_face_is_relaxed_not_sad(self):
        """sleepy 用半闭眼表达困倦，不得叠加低头、压眉和暗色造成 sad 语义。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("{55, 0, 0, 20, 42, 0, 0}", eyes_cc)
        self.assertIn("{0, 0, 0.22f, 0.02f, 0.45f, 0.02f", eyes_cc)
        self.assertNotIn("0x3E8E96,  // sleepy", eyes_cc)
        self.assertIn("0,         // sleepy：品牌青", eyes_cc)

    def test_grobot_sleepy_disables_blink_and_idle_saccade(self):
        """sleepy 保持稳定半闭眼和居中视线，退出后重新开始眨眼间隔。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("kSleepyMoodIndex", eyes_cc)
        blink_start = eyes_cc.index("void GrobotEyes::Blink()")
        blink_end = eyes_cc.index("void GrobotEyes::ApplyFacialData", blink_start)
        blink_body = eyes_cc[blink_start:blink_end]
        self.assertIn("if (mood_index_ == kSleepyMoodIndex)", blink_body)
        self.assertIn("targetL_.topH = baseL_.topH", blink_body)
        set_start = eyes_cc.index("void GrobotEyes::SetEmotion")
        set_end = eyes_cc.index("void GrobotEyes::SetSpeaking", set_start)
        set_body = eyes_cc[set_start:set_end]
        self.assertIn("last_blink_us_ = esp_timer_get_time()", set_body)
        saccade_start = eyes_cc.index("// 空闲眼跳")
        saccade_end = eyes_cc.index("ApplySpring(saccCurX_", saccade_start)
        saccade_body = eyes_cc[saccade_start:saccade_end]
        self.assertIn("mood_index_ != kSleepyMoodIndex", saccade_body)

    def test_grobot_eyebrows_use_mirrored_quadratic_curves(self):
        """眉毛不能是机械直杆：左右眉各用 12 段二次曲线，保留情绪倾角并镜像。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        start = eyes_cc.index("void GrobotEyes::DrawEyebrows")
        end = eyes_cc.index("void GrobotEyes::DrawNose", start)
        body = eyes_cc[start:end]
        self.assertIn("for (int i = 1; i <= 12; i++)", body)
        self.assertIn("1.0f - nx * nx", body)
        self.assertIn("browArch", body)
        self.assertIn("const int halfW = eyeR * 3 / 4", body)
        self.assertIn("const int browArch = std::max(10, eyeR / 6)", body)
        self.assertIn("const int browWeight = std::max(2, eyeR / 24)", body)
        self.assertIn("browGlowWeight", body)
        self.assertIn("glow_[0]", body)
        self.assertIn("brow, browWeight", body)
        self.assertIn("drawBrow", body)
        self.assertNotIn("cx - offset - halfW, yL - tiltL", body)

    def test_grobot_child_friendly_emotions_bound_extremes(self):
        """未成年人界面保留负面情绪语义，但限制攻击性角度、刺激色和剧烈动画。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("{0, 0, -32, 27, 45, 0, 0.15f}", eyes_cc)  # sad
        self.assertIn("{0, 0, 35, 30, 45, 0, 0}", eyes_cc)  # angry
        self.assertIn("{24, 0, -30, 24, 43, 0, 0.15f}", eyes_cc)  # crying
        self.assertIn("{0, 0, -6, 38, 50, 0, 0}", eyes_cc)  # shocked
        self.assertIn("{-8, -8, -0.03f, -0.03f, -0.35f, 0.05f", eyes_cc)
        self.assertIn("{18, 18, -0.04f, -0.04f, -0.20f, 0.06f", eyes_cc)
        self.assertIn("{-8, -8, -0.06f, -0.06f, -0.40f, 0.18f", eyes_cc)
        self.assertIn("{0, 0, 0.18f, 0.18f, 0, 0.72f", eyes_cc)
        self.assertIn("0xFF7A70,  // angry：柔珊瑚", eyes_cc)
        self.assertIn("0xA8CAFF,  // crying：柔浅蓝", eyes_cc)
        self.assertIn("0xFFD166,  // shocked：暖金", eyes_cc)
        self.assertIn("mood_index_ == kSleepyMoodIndex ? 0", eyes_cc)
        self.assertIn("1.0f * sinf", eyes_cc)
        self.assertNotIn("2.0f * sinf", eyes_cc)

    def test_grobot_speaking_mouth_is_clean_and_nose_has_contrast(self):
        """张嘴时只画独立嘴腔/舌色，不叠闭嘴双唇；鼻色对黑底有足够辨识度。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        nose_start = eyes_cc.index("void GrobotEyes::DrawNose")
        mouth_start = eyes_cc.index("void GrobotEyes::DrawMouth", nose_start)
        effects_start = eyes_cc.index("void GrobotEyes::DrawFacialEffects", mouth_start)
        nose_body = eyes_cc[nose_start:mouth_start]
        mouth_body = eyes_cc[mouth_start:effects_start]
        self.assertIn("bg_color_, 145", nose_body)
        self.assertIn("const int noseRx = std::max(14, eyeR / 3)", nose_body)
        self.assertIn("noseHighlight", nose_body)
        self.assertNotIn("BufFillEllipse(cx, noseY - 1", nose_body)
        self.assertIn("if (open > 5)", mouth_body)
        self.assertIn("tongue", mouth_body)
        self.assertIn("upperLipWeight", mouth_body)
        self.assertIn("lowerLipWeight", mouth_body)
        self.assertIn("return;", mouth_body)
        self.assertLess(mouth_body.index("return;"), mouth_body.index("int prevX"))

    def test_grobot_waveshare_visual_polish_contract(self):
        """Waveshare 大屏细化：降低背景竞争、收窄眼环、压紧眉鼻嘴节奏。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("for (int y = 5; y < h_; y += 6)", eyes_cc)
        self.assertNotIn("BufFillRect(0, 0, corner", eyes_cc)
        self.assertNotIn("const int corner =", eyes_cc)
        self.assertIn("eyeR + 10 + pulse", eyes_cc)
        self.assertNotIn("eyeR + 15 + pulse", eyes_cc)
        self.assertIn("const int browBaseOffset = eyeR / 10", eyes_cc)
        self.assertIn("const int halfW = eyeR * 3 / 4", eyes_cc)
        self.assertIn("const int browWeight = std::max(2, eyeR / 24)", eyes_cc)
        self.assertIn("const int noseRx = std::max(14, eyeR / 3)", eyes_cc)
        self.assertIn("noseShadow", eyes_cc)
        self.assertIn("const int width = eyeR * 11 / 5", eyes_cc)
        self.assertIn("const int lipGap = std::max(6, open / 2)", eyes_cc)
        self.assertIn("{0, 0, 0, 0, 0.48f, 0.12f", eyes_cc)
        self.assertIn("const int lipArc = (int)(curve * (1.0f - nx * nx))", eyes_cc)
        self.assertIn("const int lipSeparation = lipGap - lipGap * iabs", eyes_cc)
        self.assertIn("layout_.scale *= 1.08f", eyes_cc)
        self.assertIn("layout_.centerY = h_ / 2 + eyeR / 5", eyes_cc)
        self.assertIn("mouthCorner", eyes_cc)
        self.assertIn("const int cy = layout_.centerY", eyes_cc)
    def test_grobot_uses_cached_golden_ratio_layout(self):
        """全脸采用缓存的黄金比例构图，避免按画布高度直接把眼睛撑满。"""
        eyes_h = (ROOT / "main/boards/lichuang-dev/grobot_eyes.h").read_text(
            encoding="utf-8"
        )
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("struct FaceLayout", eyes_h)
        self.assertIn("FaceLayout layout_", eyes_h)
        self.assertIn("constexpr float kPhi = 1.61803398875f", eyes_cc)
        self.assertIn("layout_.eyeRadius = (int)lroundf(kEyeRadius * layout_.scale)", eyes_cc)
        self.assertIn("layout_.centerY = h_ / 2 + eyeR / 5", eyes_cc)
        self.assertNotIn("usableHeight / (kPhi * kPhi * kPhi)", eyes_cc)
        render_start = eyes_cc.index("void GrobotEyes::Render()")
        render_body = eyes_cc[render_start:]
        self.assertNotIn("float scale = (float)h_ / kBaseH", render_body)

    def test_grobot_review_hardening_palette_lock_init_and_tables(self):
        """审查修复：语义色真实应用；状态更新持锁；canvas 分配失败回落；
        四张情绪表由编译期不变量保证同长。"""
        eyes_h = (ROOT / "main/boards/lichuang-dev/grobot_eyes.h").read_text(
            encoding="utf-8"
        )
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")

        palette_start = eyes_cc.index("void GrobotEyes::RecomputePalette")
        palette_end = eyes_cc.index("GrobotEyes::~GrobotEyes", palette_start)
        palette_body = eyes_cc[palette_start:palette_end]
        self.assertIn("eye_color_ = eye_color", palette_body)
        self.assertIn("void GrobotEyes::UpdateDeltaTime()", eyes_cc)

        status_start = lcd_cc.index("void LcdDisplay::SetStatus")
        status_end = lcd_cc.index("void LcdDisplay::SetEmotion", status_start)
        status_body = lcd_cc[status_start:status_end]
        self.assertIn("DisplayLockGuard lock(this)", status_body)
        self.assertLess(
            status_body.index("DisplayLockGuard lock(this)"),
            status_body.index("grobot_eyes_->SetSpeaking"),
        )

        self.assertIn("bool Init(lv_obj_t* parent, int w, int h)", eyes_h)
        init_start = eyes_cc.index("bool GrobotEyes::Init")
        init_end = eyes_cc.index("void GrobotEyes::TimerCb", init_start)
        init_body = eyes_cc[init_start:init_end]
        self.assertIn("if (draw_buf_ == nullptr)", init_body)
        self.assertIn("if (canvas_ == nullptr)", init_body)
        self.assertIn("if (timer_ == nullptr)", init_body)
        self.assertIn("return false", init_body)
        self.assertIn("return true", init_body)
        self.assertIn("if (eyes->Init(emoji_box_, kFaceWidth, kFaceHeight))", lcd_cc)
        self.assertIn("Failed to initialize GrobotEyes", lcd_cc)

        self.assertIn("static_assert(std::size(kMoods) == std::size(kNames))", eyes_cc)
        self.assertIn("static_assert(std::size(kFacialMoods) == std::size(kNames))", eyes_cc)
        self.assertIn("static_assert(std::size(kMoodColors) == std::size(kNames))", eyes_cc)
        self.assertIn("kMoodCount = std::size(kNames)", eyes_cc)

if __name__ == "__main__":
    unittest.main()

