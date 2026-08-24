import json
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

    def test_preview_url_requires_png_and_draw_url_requires_gcode(self):
        """预览与机械文件必须同走能力 URL 边界，但扩展名不可互换。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"
            #include <cassert>
            int main() {
                using hutuji::IsValidDrawCapabilityUrl;
                const char* base = "https://hutuji.donglicao.com/files/draw_20260817_120000_abcdefghijklmnopqrstuv";
                assert(IsValidDrawCapabilityUrl(std::string(base) + ".gcode", ".gcode"));
                assert(IsValidDrawCapabilityUrl(std::string(base) + ".png", ".png"));
                assert(!IsValidDrawCapabilityUrl(std::string(base) + ".png", ".gcode"));
                assert(!IsValidDrawCapabilityUrl(std::string(base) + ".gcode", ".png"));
                assert(!IsValidDrawCapabilityUrl("https://evil.example/files/draw_x.png", ".png"));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_preview_url_contract_test")

    def test_draw_tool_requires_preview_then_explicit_confirmation(self):
        """hutuji.draw 只发布预览；用户确认必须走独立 hutuji.confirm。"""
        board = (ROOT / "main/boards/lichuang-dev/lichuang_dev_board.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn('Property("preview_url", kPropertyTypeString)', board)
        self.assertIn('mcp_server.AddTool("hutuji.confirm"', board)
        self.assertIn("RequestConfirm()", board)
        self.assertIn("StartDraw(url, preview_url)", board)

        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        start = job.index("std::string Job::StartDraw")
        confirm = job.index("std::string Job::RequestConfirm", start)
        start_body = job[start:confirm]
        confirm_body = job[confirm : job.index("std::string Job::RequestAbort", confirm)]
        self.assertIn('SetState("previewing")', start_body)
        self.assertNotIn('xTaskCreate(TaskEntry, "hutuji_draw"', start_body)
        self.assertIn('SetState("awaiting_confirmation")', job)
        self.assertIn('xTaskCreate(TaskEntry, "hutuji_draw"', confirm_body)


    def test_final_hil_emits_stable_ui_and_job_markers(self):
        """一次最终验收必须能从串口区分 UI 动作、Job 状态与语音状态。"""
        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        board = (
            ROOT / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        app_state = (ROOT / "main/device_state_machine.cc").read_text(encoding="utf-8")

        self.assertIn('"job state=%s"', job)
        self.assertIn('"ui preview action=confirm"', job)
        self.assertIn('"ui preview action=cancel"', job)
        self.assertIn('"ui machine action=%s"', board)
        for action in ("pause", "resume", "abort", "repeat", "pen_test"):
            self.assertIn(f'ScheduleMachineControl("{action}"', board)
        self.assertIn('"State: %s -> %s"', app_state)

    def test_long_draw_task_cannot_preempt_audio_processing(self):
        """长灌流必须让位给 AFE/编解码；abort 与 Telnet 泵仍保留快路径。"""
        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        pipe = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(encoding="utf-8")
        afe = (ROOT / "main/audio/engines/afe_audio_engine.cc").read_text(encoding="utf-8")
        audio = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")

        draw_priorities = [
            int(value)
            for value in re.findall(
                r'xTaskCreate\(TaskEntry,\s*"hutuji_draw",\s*8192,\s*this,\s*(\d+),',
                job,
            )
        ]
        self.assertEqual(len(draw_priorities), 2, "确认出图与重画必须共用调度约束")
        afe_priority = int(re.search(r'"audio_afe",\s*4096,\s*this,\s*(\d+),', afe).group(1))
        opus_priority = int(
            re.search(r'"opus_codec",\s*2048\s*\*\s*12,\s*this,\s*(\d+),', audio).group(1)
        )
        abort_priority = int(
            re.search(r'"hutuji_abort",\s*4096,\s*nullptr,\s*(\d+),', job).group(1)
        )
        pipe_priority = int(
            re.search(r'"hutuji_tcp",\s*4096,\s*this,\s*(\d+),', pipe).group(1)
        )

        self.assertTrue(all(priority < afe_priority for priority in draw_priorities))
        self.assertTrue(all(priority < opus_priority for priority in draw_priorities))
        self.assertTrue(all(abort_priority > priority for priority in draw_priorities))
        self.assertTrue(all(pipe_priority > priority for priority in draw_priorities))

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

    def test_auth_probe_stall_distinguishes_suspend_from_lost_probe(self):
        """R22-PIPE-02：`$I` 无应答必须可判定，且挂起期一律不重发。

        实测源码面：`protocol_exec_rt_suspend()`（Grbl_Esp32/src/Protocol.cpp:880）
        的 `while (sys.suspend.value)` 循环内没有 `protocol_poll_client()` 调用，
        该函数 5 个调用点全在 `protocol_main_loop` 侧。于是 Hold/Door/Sleep 期行
        命令只被收进 client_buffer 排队，`$I` 既不回 `ok` 也不回 `error`——连
        `error:8`（idleOrAlarm 门）都到不了，纯错误驱动的 R10-PIPE-01 重试永不
        触发；而实时 `?` 照常应答，silent_polls 每拍被状态行清零、keepalive 也
        不触发。结果是 WaitingBuildInfoOk 无声挂死，连接活着但永不 ready。

        修复口径：按 recv 超时拍数判无声超时。挂起 → ParkSuspended（只播报并冻结，
        不重发：重发会在 client_buffer 里堆积，解除后一次性回出多个 `ok` 与后续
        M5/G53 阶段错配）；未挂起 → Reprobe（`$I` 真丢了，走有界重试）。
        挂起解除后只重置无声计数让排队的 `$I` 走完窗口，绝不补发。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>

            int main() {
                using hutuji::AuthProbeStall;
                using hutuji::DecideAuthProbeStall;
                using hutuji::GrblSuspendBlocksLines;
                using hutuji::ShouldRearmStalledProbe;

                // 挂起谓词：Hold/Door/Sleep 都阻塞行命令；Idle/Run/Alarm 不阻塞。
                assert(GrblSuspendBlocksLines(true, false, false));
                assert(GrblSuspendBlocksLines(false, true, false));
                assert(GrblSuspendBlocksLines(false, false, true));
                assert(!GrblSuspendBlocksLines(false, false, false));

                // 未到上限：无论挂起与否都继续等（`$I` 正常是毫秒级应答）。
                assert(DecideAuthProbeStall(0, 4, true) == AuthProbeStall::KeepWaiting);
                assert(DecideAuthProbeStall(3, 4, true) == AuthProbeStall::KeepWaiting);
                assert(DecideAuthProbeStall(3, 4, false) == AuthProbeStall::KeepWaiting);

                // 到上限 + 挂起：停等播报，绝不重发（buffer 堆积 → 多 ok 错配）。
                assert(DecideAuthProbeStall(4, 4, true) == AuthProbeStall::ParkSuspended);
                assert(DecideAuthProbeStall(9, 4, true) == AuthProbeStall::ParkSuspended);

                // 到上限 + 未挂起：`$I` 真丢了，按 R10-PIPE-01 有界重试重探。
                assert(DecideAuthProbeStall(4, 4, false) == AuthProbeStall::Reprobe);
                assert(DecideAuthProbeStall(99, 4, false) == AuthProbeStall::Reprobe);

                // 上限 0 = 立即判定（保守配置也必须成立，且仍分两路）。
                assert(DecideAuthProbeStall(0, 0, true) == AuthProbeStall::ParkSuspended);
                assert(DecideAuthProbeStall(0, 0, false) == AuthProbeStall::Reprobe);

                // 自愈只在「仍等 $I」且「挂起刚解除」这一个边沿上触发。
                assert(ShouldRearmStalledProbe(true, true, false));
                // 仍挂起 / 本来就没挂起 / 已不在等 $I：都不是自愈边沿。
                assert(!ShouldRearmStalledProbe(true, true, true));
                assert(!ShouldRearmStalledProbe(true, false, false));
                assert(!ShouldRearmStalledProbe(false, true, false));
                // 进入挂起（false -> true）不是自愈边沿。
                assert(!ShouldRearmStalledProbe(true, false, true));
                return 0;
            }
            """
        )

        self._compile_and_run(compiler, source, stem="hutuji_auth_probe_stall_test")

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
        for forbidden in ("ESP911", "ESP912", "ESP913", "M701", "M704", "M711",
                          "M712", "M713", "M721"):
            self.assertNotIn(forbidden, body)
        self.assertIn("user_m30()/paper_auto_change()", body)

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
        """P1：NaN/Inf 与只含 WPos 的 fresh status 都不能成为丢 ok 的位置证据。

        新鲜读协议 2026-08-20 起抽成 `QueryAndWaitFreshMachineState`（点动越界判定
        同款前置），丢 ok 兜底只认对它的调用。"""
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
        fresh_start = job_cc.index("bool Job::QueryAndWaitFreshMachineState")
        fresh_end = job_cc.index("bool Job::ConfirmInFlightDoneByStatus", fresh_start)
        fresh = job_cc[fresh_start:fresh_end]
        self.assertIn("GetMposReportSequence", fresh)
        self.assertIn("mpos_seq", fresh)
        fallback_start = job_cc.index("bool Job::ConfirmInFlightDoneByStatus")
        fallback_end = job_cc.index("bool Job::WaitForIdle", fallback_start)
        fallback = job_cc[fallback_start:fallback_end]
        # 流式兜底保持 2s 预算（`?` 不排 planner 队列）；交互点动另有 6s 预算，
        # 两者显式传参、不设默认值，避免哪一侧被悄悄改成另一侧的语义。
        self.assertIn("QueryAndWaitFreshMachineState(kOkFallbackIdleTimeoutMs)", fallback)

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
    def test_waveshare_writer_has_no_automatic_or_long_press_poweroff(self):
        """固定供电写字机禁用 PWRON 关机，且不得误关 AXP2101 过温保护。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        self.assertIn("PowerSaveTimer(-1, 180, -1)", board)
        self.assertIn("SetBrightness(35)", board)
        self.assertIn("common_config & ~0x04", board)
        self.assertIn("(poweroff_enable | 0x04) & ~0x02", board)
        self.assertNotIn("WriteReg(0x22, common_config & ~0x04)", board)
        self.assertNotIn("WriteReg(0x22, 0b110)", board)
        self.assertNotIn("WriteReg(0x27, 0x10)", board)
        self.assertNotIn("OnShutdownRequest", board)
        self.assertNotIn("pmic_->PowerOff()", board)
    def test_waveshare_keeps_dcdc1_uvp_and_uses_force_pwm(self):
        """固定供电写字机改善 DCDC1 负载阶跃，但保留欠压保护和 1.5A 输入限流。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        constructor = board[board.index("Pmic(i2c_master_bus_handle_t"):board.index("// Disable All DCs")]
        self.assertIn("const uint8_t dc_force_pwm = ReadReg(0x81);", constructor)
        self.assertIn("WriteReg(0x81, dc_force_pwm | 0x07);", constructor)
        self.assertIn("pmic dcdc1 mode=%s reg81=0x%02x", constructor)
        self.assertIn("(configured_dc_mode & 0x04) != 0", constructor)
        self.assertNotIn("WriteReg(0x15", constructor)
        self.assertNotIn("WriteReg(0x16", constructor)
        self.assertNotIn("WriteReg(0x23", constructor)
        self.assertNotIn("WriteReg(0x24", constructor)
    def test_waveshare_logs_latched_pmic_poweroff_cause_before_reconfiguration(self):
        """上电后必须先记录 AXP2101 锁存原因，再修改关机源；否则故障证据会被初始化覆盖。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        constructor = board[board.index("Pmic(i2c_master_bus_handle_t"):board.index("// Disable All DCs")]
        self.assertIn("boot_poweroff_status_ = ReadReg(0x21);", constructor)
        self.assertIn("boot_status1_ = ReadReg(0x00);", constructor)
        self.assertIn("boot_status2_ = ReadReg(0x01);", constructor)
        self.assertIn("boot_input_current_limit_ = ReadReg(0x16);", constructor)
        self.assertIn("boot_irq_status1_ = ReadReg(0x48);", constructor)
        self.assertIn("boot_irq_status2_ = ReadReg(0x49);", constructor)
        self.assertIn("boot_irq_status3_ = ReadReg(0x4A);", constructor)
        self.assertIn("LogBootStatus();", constructor)
        self.assertIn("void LogBootStatus() const", board)
        self.assertIn("pmic boot status", board)
        self.assertLess(constructor.index("ReadReg(0x21)"), constructor.index("WriteReg(0x10"))
        self.assertLess(constructor.index("ReadReg(0x21)"), constructor.index("WriteReg(0x22"))
        replay = board[board.index("void ReplayPmicBootStatusAfterUsbReady()"):
                       board.index("void InitializePowerSaveTimer()")]
        self.assertIn("pdMS_TO_TICKS(15000)", replay)
        self.assertIn("pmic->LogBootStatus();", replay)
        self.assertIn("vTaskDelete(nullptr);", replay)
        self.assertNotIn("ReadReg(", replay)
        self.assertNotIn("WriteReg(", replay)
        self.assertIn("ReplayPmicBootStatusAfterUsbReady();", board)
    def test_waveshare_clamps_output_volume_for_unbuffered_vsys(self):
        """无电池缓冲时功放是 VSYS 最大瞬态负载；启动时把音量钳到 50。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        self.assertIn("output_volume() > 50", board)
        self.assertIn("SetOutputVolume(50)", board)
    def test_waveshare_limits_wifi_tx_power_for_unbuffered_vsys(self):
        """同室部署的写字机把 Wi-Fi 发射功率降到 10dBm，削减射频峰值电流。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        self.assertIn("void StartNetwork() override", board)
        self.assertIn("WifiBoard::StartNetwork();", board)
        self.assertIn("esp_wifi_set_max_tx_power(40)", board)
    def test_hutuji_downloads_wait_for_audio_output_idle(self):
        """播报+HTTPS 下载并发是 VSYS 最大组合负载；下载前必须等播报结束。
        等待信号必须是「真在播」（speaking 态）：双工 codec 监听期间为保 RX 时钟
        永不关 output（audio_service 刻意设计），等 output_enabled() 会恒吃满 30s
        上限（2026-08-23 HIL 实测预览/G-code 两段下载各白等 30s）。"""
        job_cc = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        job_h = (ROOT / "main/boards/lichuang-dev/hutuji_job.h").read_text(encoding="utf-8")
        self.assertIn("void WaitForAudioOutputIdle();", job_h)
        start = job_cc.index("void Job::WaitForAudioOutputIdle()")
        wait_fn = job_cc[start:job_cc.index("\n}\n", start)]
        self.assertIn("kDeviceStateSpeaking", wait_fn)
        self.assertNotIn("output_enabled()", wait_fn)
        self.assertNotIn("GetAudioCodec()", wait_fn)
        self.assertIn("pdMS_TO_TICKS(100)", wait_fn)
        self.assertIn("abort_requested_", wait_fn)
        preview_fn = job_cc[job_cc.index("bool Job::DownloadAndShowPreview"):
                            job_cc.index("void Job::Run(")]
        self.assertIn("WaitForAudioOutputIdle();", preview_fn)
        self.assertLess(preview_fn.index("WaitForAudioOutputIdle();"),
                        preview_fn.index("CreateHttp"))
        gcode_fn = job_cc[job_cc.index("bool Job::DownloadToPsram"):
                          job_cc.index("bool Job::VerifyCrc")]
        self.assertIn("WaitForAudioOutputIdle();", gcode_fn)
        self.assertLess(gcode_fn.index("WaitForAudioOutputIdle();"),
                        gcode_fn.index("CreateHttp"))

    def test_toggle_chat_interrupts_speaking_into_default_listening(self):
        """点击切换钮打断播报后必须直接进入默认监听模式，而不是只静音。"""
        app = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = app.index("void Application::HandleToggleChatEvent()")
        end = app.index("void Application::ContinueOpenAudioChannel", start)
        handler = app[start:end]
        speaking_start = handler.index("} else if (state == kDeviceStateSpeaking)")
        speaking = handler[speaking_start:handler.index("} else if (state == kDeviceStateListening)", speaking_start)]
        self.assertIn("AbortSpeaking(kAbortReasonNone);", speaking)
        self.assertIn("audio_service_.ResetDecoder();", speaking)
        self.assertIn("SetListeningMode(mode);", speaking)
        # 通道已关时必须先重开再进监听（2026-08-22 HIL：无 UDP 上行永久卡死）。
        self.assertIn("protocol_->IsAudioChannelOpened()", speaking)
        self.assertIn("ContinueOpenAudioChannel(mode);", speaking)
        # speaking→connecting 非合法边，重开路径必须经 idle 中转。
        self.assertIn("SetDeviceState(kDeviceStateIdle);", speaking)
        self.assertIn("SetDeviceState(kDeviceStateConnecting);", speaking)
        self.assertLess(
            speaking.index("AbortSpeaking(kAbortReasonNone);"),
            speaking.index("audio_service_.ResetDecoder();"),
        )
        self.assertLess(
            speaking.index("audio_service_.ResetDecoder();"),
            speaking.index("SetListeningMode(mode);"),
        )

    def test_mqtt_session_goodbye_and_stale_hello_guards(self):
        """goodbye 排队关闭须复核会话；空通道关闭无副作用；迟到 hello 被丢弃。

        2026-08-22 HIL 坐实：WiFi 抖动后用户点「说话」，新会话开启 160ms 即被
        排队的旧 goodbye 关闭掐断；超时放弃后的迟到 hello 还会污染下一次开启。
        """
        mqtt_cc = (ROOT / "main/protocols/mqtt_protocol.cc").read_text(encoding="utf-8")
        mqtt_h = (ROOT / "main/protocols/mqtt_protocol.h").read_text(encoding="utf-8")

        goodbye = mqtt_cc[mqtt_cc.index('strcmp(type->valuestring, "goodbye")'):]
        goodbye = goodbye[: goodbye.index("} else if (on_incoming_json_")]
        self.assertIn("std::string goodbye_session = session_id->valuestring;", goodbye)
        self.assertIn("session_id_ == goodbye_session", goodbye)

        close_fn = mqtt_cc[mqtt_cc.index("void MqttProtocol::CloseAudioChannel"):]
        close_fn = close_fn[: close_fn.index("bool MqttProtocol::OpenAudioChannel")]
        self.assertIn("if (udp == nullptr) {", close_fn)
        self.assertLess(close_fn.index("if (udp == nullptr) {"),
                        close_fn.index("if (send_goodbye) {"))

        open_fn = mqtt_cc[mqtt_cc.index("bool MqttProtocol::OpenAudioChannel"):]
        open_fn = open_fn[: open_fn.index("std::string MqttProtocol::GetHelloMessage")]
        self.assertIn("hello_pending_ = true;", open_fn)
        self.assertLess(open_fn.index("hello_pending_ = true;"),
                        open_fn.index("SendText(message)"))
        self.assertLess(open_fn.index("xEventGroupWaitBits"),
                        open_fn.rindex("hello_pending_ = false;"))

        parse_fn = mqtt_cc[mqtt_cc.index("void MqttProtocol::ParseServerHello"):]
        parse_fn = parse_fn[: parse_fn.index("bool MqttProtocol::CryptAesCtr")]
        self.assertIn("if (!hello_pending_) {", parse_fn)
        self.assertLess(parse_fn.index("if (!hello_pending_) {"),
                        parse_fn.index('cJSON_GetObjectItem(root, "transport")'))
        self.assertIn("std::atomic<bool> hello_pending_", mqtt_h)

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

    def test_waveshare_boot_functions_on_screen_and_wifi_lost_watchdog(self):
        """boot 键功能上屏 + 断连自动显二维码（2026-08-20 商业化少按键决策）。

        「说话」与 boot 单击同语义（starting 转配网，否则 ToggleChatState）；
        「配网」直接 EnterWifiConfigMode；Disconnected 起 120s 看门狗到期自动进
        配网显二维码，Connected/进配网即撤，全程不需要实体键。
        """
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        lcd_h = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")

        # 显示层：两个按钮 + 配置入口 + 与触发钮同进退的可见性（4 处联动）。
        self.assertIn("voice_talk_btn_", lcd_h)
        self.assertIn("wifi_config_btn_", lcd_h)
        self.assertIn("ConfigureVoiceEntry", lcd_h)
        self.assertIn("Lang::Strings::VOICE_TALK", lcd_cc)
        self.assertIn("Lang::Strings::WIFI_CONFIG_SHORT", lcd_cc)
        self.assertGreaterEqual(lcd_cc.count("voice_talk_btn_, LV_OBJ_FLAG_HIDDEN"), 4)
        self.assertGreaterEqual(lcd_cc.count("wifi_config_btn_, LV_OBJ_FLAG_HIDDEN"), 4)
        # 说话/配网复刻触发钮的按下跟随拖动：共享 AttachHomeEntryButton、
        # PRESS_LOCK、24px 阈值、松手未拖才触发（用户要求与「绘图控制」同款可拖）。
        self.assertIn("AttachHomeEntryButton", lcd_h)
        # （带 NVS 前缀的 4 参调用在下方布局记忆段单独钉）
        self.assertIn("lv_obj_add_flag(voice_talk_btn_, LV_OBJ_FLAG_PRESS_LOCK);", lcd_cc)
        self.assertIn("lv_obj_add_flag(wifi_config_btn_, LV_OBJ_FLAG_PRESS_LOCK);", lcd_cc)
        attach = lcd_cc[lcd_cc.index("void LcdDisplay::AttachHomeEntryButton"):]
        attach = attach[: attach.index("LV_EVENT_ALL, state);") + len("LV_EVENT_ALL, state);")]
        self.assertIn("kTriggerDragThresholdPx", attach)
        self.assertIn("LV_EVENT_PRESSING", attach)
        self.assertIn("LV_EVENT_RELEASED", attach)
        self.assertIn("state->dragging", attach)
        # user_data 必须传 state（回调按 HomeButtonDrag* 解引用）：2026-08-20 实机
        # 曾照抄触发钮传 this → LoadProhibited 白屏重启；钉死防回退。
        self.assertIn("LV_EVENT_ALL, state);", attach)
        self.assertNotIn("LV_EVENT_ALL, this);", attach)

        # 板级接线：说话 = boot 单击同语义（不重复 WakeUp——触摸钩子已做）。
        # 二维码「关闭」退出配网：Schedule 回主循环（StopConfigAp 事件回调同步，
        # 新机无凭据时 TryWifiConnect 有 1.5s delay，卡 taskLVGL），再由
        # ConfigModeExit→WifiBoard 自动回连。按钮 IGNORE_LAYOUT 绝对定位左上角，
        # 不被 320px 高 flex 列裁掉。
        self.assertIn("SetProvisioningCancelHandler", board)
        self.assertIn("Application::GetInstance().Schedule(", board)
        self.assertIn("WifiManager::GetInstance().StopConfigAp();", board)
        self.assertIn("provisioning_cancel_btn_", lcd_cc)
        self.assertIn("provisioning_on_cancel_", lcd_cc)
        self.assertIn("lv_obj_add_flag(provisioning_cancel_btn_, LV_OBJ_FLAG_IGNORE_LAYOUT);", lcd_cc)
        # 布局记忆（2026-08-20 用户决策）：三个可拖钮落点存 NVS hutuji_ui，
        # 真拖动才写、越界存档回默认、建钮时读回。
        self.assertIn('SaveHomeButtonPos("trig"', lcd_cc)
        self.assertIn('LoadHomeButtonPos("talk"', lcd_cc)
        self.assertIn('LoadHomeButtonPos("trig"', lcd_cc)
        self.assertIn('LoadHomeButtonPos("wifi"', lcd_cc)
        self.assertIn('Settings settings("hutuji_ui", true);', lcd_cc)
        # 复审 P2：点按松手回弹到按下位（PRESSING 阈值内残位移不残留，与触发钮
        # 同语义）；贴边存档（露出不足 24px）视为脏数据回默认；nvs_open 失败时
        # SetInt 告警跳过而非 ESP_ERROR_CHECK abort（settings.cc 句柄守卫）。
        self.assertIn("lv_obj_set_pos(target, state->press_x, state->press_y);", lcd_cc)
        self.assertIn("vx > LV_HOR_RES - 24", lcd_cc)
        settings_cc = (ROOT / "main/settings.cc").read_text(encoding="utf-8")
        self.assertIn('ESP_LOGW(TAG, "Namespace %s open failed, skip SetInt %s"', settings_cc)
        self.assertIn('AttachHomeEntryButton(voice_talk_btn_, &voice_talk_drag_, &voice_talk_, "talk");', lcd_cc)
        self.assertIn('AttachHomeEntryButton(wifi_config_btn_, &wifi_config_drag_, &wifi_config_, "wifi");', lcd_cc)
        entry = board[board.index("display_->ConfigureVoiceEntry("):]
        entry = entry[: entry.index(");", entry.index("EnterWifiConfigMode(); }")) + 2]
        self.assertIn("app.GetDeviceState() == kDeviceStateStarting", entry)
        self.assertIn("app.ToggleChatState();", entry)
        self.assertNotIn("WakeUp", entry)

        # 断连看门狗：120s 定时、Disconnected 武装、Connected/进配网撤除、
        # 到期 Schedule 回主循环 EnterWifiConfigMode（esp_timer 上下文不碰网络状态机）。
        self.assertIn("wifi_lost_timer_", board)
        self.assertIn("120ULL * 1000 * 1000", board)
        self.assertIn("StartWifiLostWatchdog();", board)
        self.assertIn("StopWifiLostWatchdog();", board)
        self.assertIn("Application::GetInstance().Schedule", board)
        wd = board[board.index("void StartWifiLostWatchdog()"):]
        wd = wd[: wd.index("void StopWifiLostWatchdog()")]
        self.assertIn("EnterWifiConfigMode();", wd)
        self.assertIn("esp_timer_start_once", wd)
        # 复审 P1-1：被状态门控早退后 one-shot 必须原地续表，否则「断连 120s
        # 自动显码」在 AP 关停期间静默失效；起到主循环执行的间隙已恢复连接
        # 则直接返回，不把已连上的设备踹进配网。
        self.assertIn("self->StartWifiLostWatchdog();", wd)
        self.assertIn("WifiManager::GetInstance().IsConnected()", wd)
        # 武装/撤除必须挂在网络事件包装里。
        wrapper = board[board.index("void SetNetworkEventCallback"):]
        wrapper = wrapper[: wrapper.index("void StartWifiLostWatchdog()")]
        self.assertIn("NetworkEvent::Disconnected", wrapper)
        self.assertLess(wrapper.index("NetworkEvent::Connected"), wrapper.index("HideProvisioningQr"))

        # 语言键双语齐备。
        zh = json.loads(
            (ROOT / "main/assets/locales/zh-CN/language.json").read_text(encoding="utf-8")
        )["strings"]
        en = json.loads(
            (ROOT / "main/assets/locales/en-US/language.json").read_text(encoding="utf-8")
        )["strings"]
        self.assertEqual(zh["VOICE_TALK"], "说话")
        self.assertEqual(zh["WIFI_CONFIG_SHORT"], "配网")
        self.assertIn("VOICE_TALK", en)
        self.assertIn("WIFI_CONFIG_SHORT", en)
        lang_h = (ROOT / "main/assets/lang_config.h").read_text(encoding="utf-8")
        self.assertIn("VOICE_TALK", lang_h)
        self.assertIn("WIFI_CONFIG_SHORT", lang_h)

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
        """触摸调参必须先读取板上真实 threshold/peak/rate/chip/vendor，不盲写官方示例值。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertIn("kFt6x36ThresholdReg = 0x80", body)
        self.assertIn("kFt6x36PeakThresholdReg = 0x81", body)
        self.assertIn("kFt6x36ActivePeriodReg = 0x88", body)
        self.assertIn("kFt6x36ChipIdReg = 0xA3", body)
        self.assertIn("kFt6x36VendorIdReg = 0xA8", body)
        self.assertIn("esp_lcd_panel_io_rx_param", body)
        self.assertIn(
            "Touch registers: threshold=%u peak=%u active_period=%u chip=0x%02X vendor=0x%02X",
            body,
        )

    def test_waveshare_applies_experimental_touch_threshold_with_readback(self):
        """实机默认 70；40 仍漏轻触（2026-08-20 用户反馈）降到 30，并回读验证。
        FT5x06 官方寄存器文档实值=4×寄存器值，Linux EDT 驱动接受 20–80，30 在界内。
        2026-08-21 三轮反馈仍不灵敏：驱动 demo 初始化把 THPEAK(0x81) 写死 60
        （按压瞬间峰值检测门），慢速轻触峰值过不了门、GROUP 再低也无济于事——
        GROUP 30→20（EDT 钳位下限）且 PEAK 60→40（首个保守步长），双值回读验证。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertIn("kTouchThreshold = 20", body)
        self.assertIn("kTouchPeakThreshold = 40", body)
        self.assertIn("esp_lcd_panel_io_tx_param", body)
        self.assertIn("Touch threshold tuned: %u -> %u peak %u -> %u", body)
        self.assertIn("Touch threshold tune failed", body)
        self.assertEqual(body.count("esp_lcd_panel_io_tx_param"), 2)
        self.assertIn("kFt6x36ThresholdReg, &kTouchThreshold", body)
        self.assertIn("kFt6x36PeakThresholdReg, &kTouchPeakThreshold", body)

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

    def test_waveshare_touch_mapping_pairs_with_current_display_matrix(self):
        """官方 demo 的 display(1,1,1) 配 touch(1,0,1)：swap 相同、触摸 X
        与显示 X 反向、Y 同向。本仓 display 是 (1,0,0)，故 touch 必须是 (1,1,0)。"""
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
        self.assertIn(".mirror_x = 1", body)
        self.assertIn(".mirror_y = 0", body)

    def test_waveshare_touch_indev_read_period_is_10ms(self):
        """LV_DEF_REFR_PERIOD=33（sdkconfig 实测）时 indev 默认 33ms 才读一次触摸，
        是「不跟手」主延迟源（LVGL issue #8152 同因）；FT6336 INT 未接 GPIO 只能
        轮询，显式 10ms 一读。0x88 芯片采样周期 12 已是官方最小推荐，不降。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        start = board.index("void InitializeTouch()")
        end = board.index("void InitializeLcdDisplay()", start)
        body = board[start:end]
        self.assertIn("lv_timer_set_period(lv_indev_get_read_timer(touch_indev), 10);", body)

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
        # 4 处：StartDraw / RequestRepeat / RequestPenTest / RequestManualControl。
        self.assertEqual(source.count('JsonString("写字机正忙，请稍候再试")'), 4)
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

        # 情绪配色：所有眼色从 π logo 共享渐变的位置表导出，禁止回到独立色相。
        self.assertIn("kMoodGradientT", eyes_cc)
        self.assertRegex(eyes_cc, r"0\.02f,\s+// angry：热粉起点")
        self.assertRegex(eyes_cc, r"0\.06f,\s+// loving：粉紫")
        self.assertRegex(eyes_cc, r"0\.32f,\s+// thinking：紫")
        self.assertIn("RecomputePalette(mood_t)", eyes_cc)

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
        self.assertIn("buf_[row * s + cl] = Blend565(Resolve(p, cl, row), buf_[row * s + cl], frac)", eyes_cc)

        # 配色族系：21 个情绪各自落在 π 的同一条渐变上，喜悦族占冷端青/薄荷，
        # 不再引入渐变外的暖金/橙/珊瑚色。
        self.assertIn("static constexpr float kMoodGradientT[]", eyes_cc)
        for entry in (r"0\.99f,\s+// happy", r"1\.00f,\s+// laughing",
                      r"0\.94f,\s+// funny", r"0\.90f,\s+// winking"):
            self.assertRegex(eyes_cc, entry)
        self.assertNotIn("0xFFCF3F", eyes_cc)
        self.assertNotIn("0xFF6B35", eyes_cc)



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
        self.assertIn("grobot_stage_ = lv_obj_create(screen)", lcd_cc)
        self.assertIn("lv_obj_set_style_radius(grobot_stage_, 32, 0)", lcd_cc)
        # 触摸落在 canvas 时，子对象和其滚动链都必须关闭；否则脸会被弹性滚动拖走。
        self.assertIn("lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE)", lcd_cc)
        self.assertIn("lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLL_CHAIN)", lcd_cc)
        self.assertIn("lv_obj_clear_flag(emoji_box_, LV_OBJ_FLAG_SCROLLABLE)", lcd_cc)
        self.assertIn("lv_obj_clear_flag(emoji_box_, LV_OBJ_FLAG_SCROLL_CHAIN)", lcd_cc)
        self.assertIn("lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE)", eyes_cc)
        self.assertIn("lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLL_CHAIN)", eyes_cc)
        self.assertIn("theme->surface_color()", lcd_cc)
        self.assertNotIn("otto_emoji_gif", eyes_cc)
        self.assertNotIn("emote::EmoteDisplay", lcd_cc)

    def test_pi_palette_drives_theme_buttons_and_grobot(self):
        """常驻按钮与 Grobot 默认色必须共用 π 品牌锚点，旧青色不可覆盖新主题。"""
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        core_h = (ROOT / "main/boards/lichuang-dev/hutuji_pi_splash_core.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("kPiBrandGradientT = 0.50f", core_h)
        self.assertIn("PiGradientHex(kPiBrandGradientT)", lcd_cc)
        self.assertIn("dark_accent = light_accent", lcd_cc)
        self.assertIn("dark_success = lv_color_hex(PiGradientHex(kPiSuccessGradientT))", lcd_cc)
        self.assertIn("dark_warning = lv_color_hex(PiGradientHex(kPiWarningGradientT))", lcd_cc)
        self.assertIn("dark_danger = lv_color_hex(PiGradientHex(kPiDangerGradientT))", lcd_cc)
        self.assertEqual(lcd_cc.count("dark_theme->set_accent_color("), 1)
        self.assertEqual(lcd_cc.count("dark_theme->set_warning_color("), 1)
        self.assertEqual(lcd_cc.count("dark_theme->set_danger_color("), 1)
        self.assertIn("RecomputePalette(kMoodGradientT[0])", eyes_cc)
        self.assertRegex(eyes_cc, r"0\.62f,\s+// sad：长春花偏青")
        self.assertNotIn("set_accent_color(lv_color_hex(0x32D6CB))", lcd_cc)
        self.assertNotIn("set_accent_color(lv_color_hex(0x0F8F8A))", lcd_cc)


    def test_grobot_full_face_shows_compact_response_subtitle(self):
        """全脸必须用独立单行字幕层显示语音/工具反馈，不依赖某个消息样式
        是否创建 bottom_bar_，也不恢复会覆盖嘴巴的大型聊天气泡。"""
        lcd_h = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        self.assertIn("grobot_subtitle_bar_", lcd_h)
        self.assertIn("grobot_subtitle_label_", lcd_h)
        self.assertIn("void SetGrobotSubtitle", lcd_h)

        init_start = lcd_cc.index("void LcdDisplay::InitializeEmotionUi")
        init_end = lcd_cc.index("void LcdDisplay::SetupUI", init_start)
        init_body = lcd_cc[init_start:init_end]
        self.assertIn("grobot_subtitle_bar_ = lv_obj_create(screen)", init_body)
        self.assertIn("grobot_subtitle_label_ = lv_label_create(grobot_subtitle_bar_)", init_body)
        self.assertIn("LV_LABEL_LONG_DOT", init_body)
        self.assertIn("lv_obj_set_size(grobot_subtitle_bar_, LV_HOR_RES * 72 / 100, 40)", init_body)
        self.assertIn("LV_OPA_80", init_body)

        helper_start = lcd_cc.index("void LcdDisplay::SetGrobotSubtitle")
        helper_end = lcd_cc.index("void LcdDisplay::SetChatMessage", helper_start)
        helper = lcd_cc[helper_start:helper_end]
        self.assertIn("lv_obj_remove_flag(grobot_subtitle_bar_, LV_OBJ_FLAG_HIDDEN)", helper)
        self.assertIn("lv_obj_add_flag(grobot_subtitle_bar_, LV_OBJ_FLAG_HIDDEN)", helper)

        bodies = lcd_cc.split("void LcdDisplay::SetChatMessage")[1:]
        self.assertGreaterEqual(len(bodies), 2)
        for body in bodies[:2]:
            self.assertIn("if (grobot_eyes_ != nullptr)", body)
            self.assertIn("SetGrobotSubtitle(content)", body)
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
        self.assertIn("scan_color_ = lv_color_mix(center_eye, bg_color_, 7)", eyes_cc)
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
        self.assertRegex(eyes_cc, r"0\.44f,\s+// sleepy：长春花偏紫")

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
        self.assertIn("Shade(kShadeGlow0)", body)
        self.assertIn("Shade(kShadeBrow), browWeight", body)
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
        # 负面情绪仍有区分，但颜色也必须留在 π 的粉→紫→青渐变：
        # angry 暖端、sad 长春花偏青、crying 青蓝、shocked 冷端。
        self.assertRegex(eyes_cc, r"0\.02f,\s+// angry：热粉起点")
        self.assertRegex(eyes_cc, r"0\.62f,\s+// sad：长春花偏青")
        self.assertRegex(eyes_cc, r"0\.70f,\s+// crying：青蓝")
        self.assertRegex(eyes_cc, r"0\.66f,\s+// shocked：青蓝")
        self.assertNotIn("0xFF7A70", eyes_cc)
        self.assertNotIn("0xFFD166", eyes_cc)
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
        self.assertIn("lv_color_mix(eye, bg_color_, 145)", eyes_cc)
        self.assertIn("const int noseRx = std::max(14, eyeR / 3)", nose_body)
        self.assertIn("Shade(kShadeNoseHi)", nose_body)
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
        self.assertIn("kShadeNoseShadow", eyes_cc)
        self.assertIn("const int width = eyeR * 11 / 5", eyes_cc)
        self.assertIn("const int lipGap = std::max(6, open / 2)", eyes_cc)
        self.assertIn("{0, 0, 0, 0, 0.48f, 0.12f", eyes_cc)
        self.assertIn("const int lipArc = (int)(curve * (1.0f - nx * nx))", eyes_cc)
        self.assertIn("const int lipSeparation = lipGap - lipGap * iabs", eyes_cc)
        self.assertIn("layout_.scale *= 1.08f", eyes_cc)
        self.assertIn("layout_.centerY = h_ / 2 + eyeR / 5", eyes_cc)
        self.assertIn("kShadeMouthCorner", eyes_cc)
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
        self.assertIn("PiGradientRgb(tc, shine_strength, shine_pos", palette_body)
        self.assertIn("kPiFacePhaseSwing", palette_body)
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
        self.assertIn("static_assert(std::size(kMoodGradientT) == std::size(kNames))", eyes_cc)
        self.assertIn("kMoodCount = std::size(kNames)", eyes_cc)

    def test_grobot_face_shine_flows_and_accent_buttons_breathe(self):
        """脸的「活」= 色相潮汐（5s 正弦 ±0.22，钳制映射无接缝）+ 上游 shine 扫光
        （说话 1.5s/周 0.90 连续扫、空闲 6s 周期占空 34%、sleepy 全停）；主屏 accent
        按钮 8s ±0.10 呼吸，说话大圆钮叠 2.5s 光晕脉动，安全语义色不参与。"""
        eyes_cc = (ROOT / "main/boards/lichuang-dev/grobot_eyes.cc").read_text(
            encoding="utf-8"
        )
        eyes_h = (ROOT / "main/boards/lichuang-dev/grobot_eyes.h").read_text(
            encoding="utf-8"
        )
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        # 潮汐 + 扫光：钳制映射（禁绕回硬边），每帧重建 64 级 LUT
        self.assertIn("BuildShadeLut", eyes_h)
        self.assertIn("std::clamp(base + phase, 0.0f, 1.0f)", eyes_cc)
        self.assertNotIn("PiWrap01(base + phase)", eyes_cc)
        self.assertIn("0.22f * sinf", eyes_cc)
        self.assertIn("PiGradientRgb(tc, shine_strength, shine_pos", eyes_cc)
        self.assertIn("kPiShineHalfWidth", eyes_cc)
        self.assertIn("speaking_ ? 1500000 : 6000000", eyes_cc)
        self.assertIn("speaking_ ? 1.0f : 0.34f", eyes_cc)
        self.assertIn("speaking_ ? 0.90f : 0.85f", eyes_cc)
        self.assertIn("mood_index_ != kSleepyMoodIndex", eyes_cc)
        self.assertIn("BuildShadeLut(mood_base_phase_ + tide", eyes_cc)
        # 按钮呼吸 + 说话钮光晕：只刷 accent 系，安全语义色恒定
        self.assertIn("AccentDriftTimerCb", lcd_cc)
        self.assertIn("0.10f * sinf", lcd_cc)
        self.assertIn("kPiBrandGradientT + drift", lcd_cc)
        self.assertIn("lv_obj_set_style_shadow_width(self->voice_talk_btn_", lcd_cc)
        self.assertIn("tick % 2500", lcd_cc)
        self.assertIn("lv_timer_create(AccentDriftTimerCb, 100, this)", lcd_cc)
        self.assertIn("lv_timer_delete(accent_drift_timer_)", lcd_cc)

    def test_production_board_registers_preview_confirm_tools(self):
        """产品板是 Waveshare：预览/确认必须在这块板上注册，否则真机走不到确认门。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        self.assertIn('Property("preview_url", kPropertyTypeString)', board)
        self.assertIn('mcp_server.AddTool("hutuji.confirm"', board)
        self.assertIn("StartDraw(url, preview_url)", board)
        self.assertIn("RequestConfirm()", board)
        # 单参 StartDraw 会编译失败，但更要防「只注册 draw 不注册 confirm」：
        # 那样预览停在 awaiting_confirmation，语音无法确认，只能点屏幕。
        self.assertEqual(board.count('mcp_server.AddTool("hutuji.confirm"'), 1)

    def test_draw_preview_overlay_has_touch_confirm_and_cancel_buttons(self):
        """预览层必须自带「开始画」「取消」按钮：语音确认可能听不清，
        触摸是唯一确定入口。按钮命中高度须有 56px 下限（儿童手指），
        回调不得在 LVGL 事件里直接跑网络/Telnet 动作。"""
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        ui_start = lcd_cc.index("void LcdDisplay::EnsureDrawPreviewUi")
        ui_end = lcd_cc.index("void LcdDisplay::ShowDrawPreview", ui_start)
        ui_body = lcd_cc[ui_start:ui_end]
        self.assertIn("lv_button_create(button_row)", ui_body)
        self.assertNotIn("LV_EVENT_CLICKED", ui_body)
        self.assertGreaterEqual(ui_body.count("LV_EVENT_PRESSED"), 2)
        self.assertRegex(ui_body, r"draw_preview_confirm_btn_\s*=\s*\n?\s*make_button")
        self.assertRegex(ui_body, r"draw_preview_cancel_btn_\s*=\s*\n?\s*make_button")
        self.assertIn("Lang::Strings::DRAW_PREVIEW_CONFIRM", ui_body)
        self.assertIn("Lang::Strings::DRAW_PREVIEW_CANCEL", ui_body)
        # 命中高度下限 56px：3.5" 320x480 上更小的按钮儿童点不准。
        self.assertIn("< 56 ? 56 :", ui_body)
        self.assertIn("lv_obj_set_size(btn, width, button_height)", ui_body)
        # 「开始画」是唯一主动作：确认键必须比取消键宽（2/3 vs 1/3）。
        self.assertIn("confirm_width = (row_width - theme->spacing(4)) * 2 / 3", ui_body)
        self.assertIn("theme->accent_color()", ui_body)
        self.assertIn("theme->assistant_bubble_color()", ui_body)
        # 提示语置顶单行截断，再长也不挤压图片与按钮。
        self.assertIn("LV_LABEL_LONG_DOT", ui_body)
        # 主题字体会被 SetTextFont 替换释放：预览层不得持有主题字体裸指针。
        self.assertNotIn("lv_obj_set_style_text_font", ui_body)
        # 预览层建好即隐藏，不得一创建就盖住脸/聊天。
        self.assertIn("lv_obj_add_flag(draw_preview_root_, LV_OBJ_FLAG_HIDDEN)", ui_body)

        hide_start = lcd_cc.index("void LcdDisplay::HideDrawPreview")
        hide_body = lcd_cc[hide_start : lcd_cc.index("LcdDisplay::~LcdDisplay", hide_start)]
        # 撤图必须同时清空回调：否则迟到点击会打到已失效的 Job 状态。
        self.assertIn("draw_preview_on_confirm_ = nullptr", hide_body)
        self.assertIn("draw_preview_on_cancel_ = nullptr", hide_body)

        lcd_h = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        self.assertIn("std::function<void()> draw_preview_on_confirm_", lcd_h)

        self.assertIn("std::function<void()> draw_preview_on_cancel_", lcd_h)

        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        preview_start = job.index("bool Job::DownloadAndShowPreview")
        preview_end = job.index("bool Job::VerifyCrc", preview_start)
        preview_body = job[preview_start:preview_end]
        self.assertIn("ShowDrawPreview", preview_body)
        # 按钮回调只能 Schedule 到主循环，不得在 LVGL 事件里直接发 Telnet。
        self.assertIn("Application::GetInstance().Schedule", preview_body)
        self.assertIn("RequestConfirm()", preview_body)
        self.assertIn("RequestAbort()", preview_body)

    def test_draw_preview_strings_exist_in_both_locales(self):
        """预览提示与按钮文案走 locale：硬编码中文会让 en-US 镜像编译不出。"""
        for locale in ("zh-CN", "en-US"):
            data = json.loads(
                (ROOT / f"main/assets/locales/{locale}/language.json").read_text(
                    encoding="utf-8"
                )
            )
            strings = data["strings"]
            for key in ("DRAW_PREVIEW_HINT", "DRAW_PREVIEW_CONFIRM", "DRAW_PREVIEW_CANCEL"):
                self.assertIn(key, strings, f"{locale} missing {key}")
                self.assertTrue(strings[key].strip(), f"{locale} {key} empty")

    def test_waveshare_machine_control_drawer_is_safe_and_touchable(self):
        """屏幕控制复用 Job API；禁危险归位/点动，LVGL 回调只调度主循环。"""
        lcd_h = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        board = (
            ROOT / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")

        self.assertIn("ConfigureMachineControls", lcd_h)
        self.assertIn("UpdateMachineControlState", lcd_h)
        self.assertIn("EnsureMachineControlUi", lcd_h)
        # ui_body 只切「构建函数本体」：切到 ConfigureMachineControls 会把 setter 与
        # ApplyMachineControlState 一并吞进来，令「创建处只折叠一次」这类计数式断言
        # 被别处的同名调用干扰（2026-08-20 实测 count 由 1 变 2 假红）。
        ui_start = lcd_cc.index("void LcdDisplay::EnsureMachineControlUi")
        ui_end = lcd_cc.index("void LcdDisplay::SetMachineDrawerPage", ui_start)
        ui_body = lcd_cc[ui_start:ui_end]
        for member in (
            "machine_pause_btn_",
            "machine_resume_btn_",
            "machine_abort_btn_",
            "machine_repeat_btn_",
            "machine_pen_test_btn_",
        ):
            self.assertIn(member, ui_body)
        self.assertIn("button_height < 56 ? 56 : button_height", ui_body)
        # 默认视觉：说话大圆钮中下、触发/配网 48px 小方钮右上（2026-08-20 美观
        # 改版，主角/角落分层）；对象采用 TOP_LEFT 坐标系，非 TOP_LEFT 对齐与
        # set_pos 增量混用会让 LVGL 坐标损坏、按钮飞出屏幕。
        self.assertIn("LV_ALIGN_TOP_LEFT", ui_body)
        self.assertIn("const lv_coord_t corner_btn_size = 48;", ui_body)
        self.assertIn("const lv_coord_t talk_diameter = 96;", ui_body)
        self.assertRegex(
            ui_body,
            r"LV_HOR_RES - 90 - theme->spacing\(3\),\s*theme->spacing\(3\)\);",
        )
        trigger_setup = ui_body[: ui_body.index("lv_obj_add_event_cb")]
        self.assertNotIn("lv_obj_get_width(machine_control_trigger_btn_)", trigger_setup)
        self.assertNotIn("lv_obj_get_height(machine_control_trigger_btn_)", trigger_setup)
        self.assertIn("LV_EVENT_PRESSING", ui_body)
        self.assertIn("LV_EVENT_RELEASED", ui_body)
        self.assertIn("lv_event_get_indev(e)", ui_body)
        self.assertIn("lv_indev_get_point", ui_body)
        self.assertNotIn("lv_indev_get_vect", ui_body)
        self.assertIn("machine_trigger_press_point_", lcd_h)
        self.assertIn("machine_trigger_press_x_", lcd_h)
        self.assertIn("machine_trigger_press_y_", lcd_h)
        self.assertIn("machine_trigger_dragging_", lcd_h)
        self.assertIn("LV_OBJ_FLAG_PRESS_LOCK", ui_body)
        self.assertIn("lv_obj_clear_flag(machine_control_trigger_btn_, LV_OBJ_FLAG_SCROLLABLE)", ui_body)
        self.assertIn("const lv_coord_t dx = point.x - self->machine_trigger_press_point_.x", ui_body)
        self.assertIn("const lv_coord_t dy = point.y - self->machine_trigger_press_point_.y", ui_body)
        # 2026-08-20 HIL：6px 在灵敏化触摸上把点按误判成拖动（抽屉打不开），钉 24px。
        self.assertIn("kTriggerDragThresholdPx = 24", lcd_cc)
        self.assertIn("lv_obj_get_x_aligned", ui_body)
        self.assertIn("lv_obj_get_y_aligned", ui_body)
        self.assertIn("kTriggerDragThresholdPx", ui_body)
        # 抽屉必须有标题与当前状态，且状态由 ApplyMachineControlState 驱动。
        self.assertIn("Lang::Strings::MACHINE_DRAWER_TITLE", ui_body)
        self.assertIn("machine_state_label_", ui_body)
        self.assertIn("machine_state_label_", lcd_h)
        # disabled 用填充色差异（灰底）表达，不靠低对比文字。
        self.assertIn("LV_STATE_DISABLED", ui_body)
        self.assertIn("machine controls opened", ui_body)
        # 面板内按钮一律 CLICKED：从按钮起手拖成滚动时 LVGL 只发 PRESS_LOST、
        # 释放时不发 CLICKED（lv_indev.c release 分支 `scroll_obj == NULL` 才发），
        # reset/set_origin 等高危键天然免疫滚动误触（2026-08-20 修复）；本抽屉
        # 不再注册任何 PRESSED 回调（触发钮走 LV_EVENT_ALL，不算）。
        # 免疫前提：板级 indev 滚动阈值提到 24px——LVGL 默认 10px
        # （LV_INDEV_DEF_SCROLL_LIMIT，lv_indev.c:1375 越限即 PRESS_LOST），FT5x06
        # 调敏后静止按压抖动越限、CLICKED 全数被吃（2026-08-20 HIL 坐实：展开态
        # 6 分钟仅 1 次折叠态 CLICKED 登记）。断 SCROLL_CHAIN 的反方案因按钮铺满
        # 行、面板无处起手滚动（X/Y 十字不可达），被同日第三轮 HIL 否决、禁复活。
        self.assertIn("lv_indev_set_scroll_limit(touch_indev, 24)", board)
        self.assertNotIn("lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN)", ui_body)
        self.assertEqual(ui_body.count("LV_EVENT_PRESSED, this)"), 0)
        self.assertGreaterEqual(ui_body.count("LV_EVENT_CLICKED, this)"), 8)
        self.assertIn("LV_OBJ_FLAG_HIDDEN", ui_body)
        self.assertIn("lv_screen_active()", ui_body)
        self.assertGreaterEqual(lcd_cc.count("lv_obj_add_flag(machine_control_trigger_btn_, LV_OBJ_FLAG_HIDDEN)"), 2)
        self.assertIn("machine_manual_section_", ui_body)
        self.assertIn("machine_manual_toggle_btn_", ui_body)
        # 维护页（第三页）：写字机零接触配网手动入口挂在抽屉维护页。
        self.assertIn("machine_maint_section_", ui_body)
        self.assertIn("machine_reprovision_btn_", ui_body)
        self.assertIn("Lang::Strings::MACHINE_REPROVISION", ui_body)
        self.assertIn("SetMachineDrawerPage(0)", ui_body)
        self.assertIn("SetMachineDrawerPage(", ui_body)
        # 展开态跨抽屉开合保持（2026-08-20 用户决策）：开机默认主页只剩创建处一处，
        # 打开抽屉不再强制回主页，连续点动无需反复切页。
        self.assertEqual(ui_body.count("SetMachineDrawerPage(0)"), 1)
        # 切页循环 主页→手动→维护→主页；EXPAND 文案在构建处（初始态），
        # 其余页文案只在切页 setter 里出现，故后者对 setter_body 断言（见下）。
        self.assertIn("(self->machine_page_ + 1) % 3", ui_body)
        self.assertIn("Lang::Strings::MACHINE_MANUAL_EXPAND", ui_body)
        # 标题行必须 locale-proof：en-US 下固定件（状态胶囊 + 切换 129 + 收起 96 +
        # 3*8 间距）已占 ~324px，标题「Drawing Controls」按 SPACE_BETWEEN 排会溢出
        # 432 成负间距并与状态胶囊重叠（lv_flex.c place_content）。标题必须 grow=1
        # 吃余量 + DOTS 截断；grow 使 SPACE_BETWEEN 余量归零，故 header 间距要显式
        # 给 pad_column。zh-CN HIL 装得下不代表 en-US 装得下，故按源码钉死。
        self.assertIn("lv_obj_set_flex_grow(title, 1)", ui_body)
        self.assertIn("lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS)", ui_body)
        self.assertIn("lv_obj_set_style_pad_column(header, theme->spacing(4), 0)", ui_body)
        # 抽屉分页（2026-08-20 第五轮 HIL 后定案）：面板固定高、不可滚动，主操作区与
        # 手动区互斥显示。旧实现 height=LV_SIZE_CONTENT + max_height 下超出视口的部分
        # 被裁掉不画；阻断滚动的具体 LVGL 环节未逐一定位，但用户连续四轮实测按钮
        # 依次丢 X/Y 十字、Y+、整段工具键，最后一轮明确「也不能滑动」——结论确定：
        # 依赖面板滚动到达满铺按钮之外内容的方案在 480x320 上不可交付，故禁复活。
        self.assertIn("lv_obj_set_height(panel, LV_VER_RES - theme->spacing(4))", ui_body)
        self.assertNotIn("lv_obj_set_height(panel, LV_SIZE_CONTENT)", ui_body)
        self.assertNotIn("lv_obj_set_style_max_height(panel", ui_body)
        self.assertNotIn("lv_obj_set_scroll_dir(panel", ui_body)
        self.assertNotIn("lv_obj_set_scrollbar_mode(panel", ui_body)
        self.assertIn("lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE)", ui_body)
        # 面板内不留任何滚动依赖：抽屉构建与切页里 scroll_to_* 一律不得复活
        # （分页已保证可达；聊天区等其它 UI 的滚动与本抽屉无关，故只查这两段）。
        self.assertNotIn("lv_obj_scroll_to_view_recursive(", ui_body)
        self.assertNotIn("lv_obj_scroll_to_y(panel", ui_body)
        setter_start = lcd_cc.index("void LcdDisplay::SetMachineDrawerPage")
        setter_end = lcd_cc.index("void LcdDisplay::ApplyMachineControlState", setter_start)
        setter_body = lcd_cc[setter_start:setter_end]
        self.assertNotIn("lv_obj_scroll_to", setter_body)
        # 切页 setter 必须三页都改文案，否则按钮标签与当前页脱节。
        self.assertIn("Lang::Strings::MACHINE_MANUAL_EXPAND", setter_body)
        self.assertIn("Lang::Strings::MACHINE_MAINT_EXPAND", setter_body)
        self.assertIn("Lang::Strings::MACHINE_MAIN_PAGE", setter_body)
        # 互斥：手动区/维护区显示则主区隐藏，反之亦然。
        self.assertIn("machine_main_section_", lcd_h)
        self.assertIn("lv_obj_add_flag(machine_main_section_, LV_OBJ_FLAG_HIDDEN)", setter_body)
        self.assertIn("lv_obj_remove_flag(machine_main_section_, LV_OBJ_FLAG_HIDDEN)", setter_body)
        self.assertIn("lv_obj_add_flag(machine_manual_section_, LV_OBJ_FLAG_HIDDEN)", setter_body)
        self.assertIn("lv_obj_remove_flag(machine_manual_section_, LV_OBJ_FLAG_HIDDEN)", setter_body)
        self.assertIn("lv_obj_add_flag(machine_maint_section_, LV_OBJ_FLAG_HIDDEN)", setter_body)
        self.assertIn("lv_obj_remove_flag(machine_maint_section_, LV_OBJ_FLAG_HIDDEN)", setter_body)
        # 主页三行都挂在主区（不是 panel）：否则切页时它们不会跟着隐藏。
        self.assertIn("make_row(machine_main_section_)", ui_body)
        self.assertIn("make_button(machine_main_section_, Lang::Strings::MACHINE_STOP", ui_body)
        # 手动页两列布局：左列点动十字（3×56 方键），右列 6 个工具键，3 行 ×56 = 184
        # ≤ 232 可用高，无需滚动即全可见。
        self.assertIn("LV_FLEX_FLOW_ROW", ui_body)
        self.assertIn("const lv_coord_t jog_size = safe_button_height", ui_body)
        self.assertIn("jog_size * 3 + theme->spacing(4) * 2", ui_body)
        self.assertIn("content_width - jog_col_width - theme->spacing(4)", ui_body)
        for action in ("jog_step_1", "jog_y+", "jog_step_10", "jog_x-", "home", "jog_x+", "jog_y-"):
            self.assertIn(f'"{action}"', ui_body)
        self.assertIn("FormatMachineHud", lcd_cc)
        self.assertIn("machine_hud_timer_", lcd_h)
        self.assertIn("lv_timer_create(MachineHudTimerCb, 400", lcd_cc)
        # 2026-08-22 实机截图：HUD 塞进标题胶囊后长串把 432px 行挤爆，方向键变飘字、
        # Y- 被裁、抬笔/落笔重影。坐标条必须在手动页全宽，标题行只走短 locale。
        self.assertIn("machine_hud_label_", lcd_h)
        self.assertIn("lv_obj_set_width(machine_hud_label_, content_width)", lcd_cc)
        self.assertIn("lv_label_set_text(machine_state_label_, state_text)", lcd_cc)
        self.assertIn("lv_label_set_text(machine_hud_label_, hud)", lcd_cc)
        self.assertNotIn("lv_label_set_text(machine_state_label_, hud)", lcd_cc)
        # 点动键描边：assistant_bubble 贴表面色时只剩白字，截图里 1mm/Y+/X± 像飘字。
        self.assertIn("lv_obj_set_style_border_width(btn, 2, 0)", ui_body)
        # 断连不得假装 Idle：状态栏通知在抽屉遮罩下面，用户点 XY 会以为没反应。
        self.assertIn("Lang::Strings::MACHINE_PLOTTER_OFFLINE", lcd_cc)
        self.assertIn("hud_pipe.IsConnected()", lcd_cc)
        self.assertIn("machine_notice_label_", lcd_h)
        self.assertIn("void LcdDisplay::ShowNotification", lcd_cc)
        self.assertIn("using LvglDisplay::ShowNotification", lcd_h)
        # 页切换钮进标题行：独占一行会吃掉 64px，主页就装不下 64+56+56 三行。
        # 只钉「父对象是 header」这一语义，不钉 clang-format 的换行位置。
        toggle_start = ui_body.index("machine_manual_toggle_btn_ =")
        toggle_args = ui_body[toggle_start:ui_body.index(";", toggle_start)]
        self.assertRegex(toggle_args, r"make_button\(\s*header,")
        self.assertIn("Lang::Strings::MACHINE_MANUAL_EXPAND", toggle_args)
        # active 态（含 streaming/paper_change）必须回主页，否则停止钮随主区一起被
        # 隐藏，而副页键此刻全灰——屏上会一个可用键都没有。判据须是 active，用
        # !settled 会把 manual（点动执行中）也踢回主页，点一下就跳页。
        apply_start = lcd_cc.index("void LcdDisplay::ApplyMachineControlState")
        apply_body = lcd_cc[apply_start:]
        self.assertIn("if (active && machine_page_ != 0)", apply_body)
        self.assertIn("SetMachineDrawerPage(0);", apply_body)
        self.assertNotIn("if (!settled && machine_manual_section_", apply_body)
        # 维护页入口仅 settled 态可用：配网跳窗会断户网，不能打断在途任务。
        self.assertIn("set_enabled(machine_reprovision_btn_, settled);", apply_body)

        # taskLVGL 栈：默认 7168 装不下手动页（比主页深两级、四级 LV_SIZE_CONTENT
        # 叠 flex），2026-08-20 实机首帧展开即 "stack overflow in task taskLVGL"
        # + rst:0xc（elf SHA 61f8ae22e）。抬到 12288 并在切页日志带 HWM，回退到
        # 默认或删掉 HWM 都会让这次崩溃静默复活。
        spi_start = lcd_cc.index("SpiLcdDisplay::SpiLcdDisplay")
        spi_body = lcd_cc[spi_start:lcd_cc.index("RgbLcdDisplay::RgbLcdDisplay", spi_start)]
        self.assertIn("port_cfg.task_stack = 12288;", spi_body)
        self.assertIn("lvgl stack hwm", setter_body)
        self.assertIn("uxTaskGetStackHighWaterMark(nullptr)", setter_body)
        self.assertIn("#include <freertos/task.h>", lcd_cc)

        self.assertIn("display_->ConfigureMachineControls", board)
        # 维护页「重新配置写字机」接零接触配网手动入口（hutuji::PlotterProvision）。
        self.assertIn("PlotterProvision::GetInstance().RequestManual()", board)
        for request in (
            "RequestPause()",
            "RequestResume()",
            "RequestAbort()",
            "RequestRepeat()",
            "RequestPenTest()",
        ):
            self.assertIn(request, board)
        self.assertIn("void ScheduleMachineControl", board)
        self.assertIn("Application::GetInstance().Schedule", board)
        self.assertGreaterEqual(board.count('ScheduleMachineControl("'), 5)
        self.assertIn("UpdateMachineControlState", job)
        self.assertNotIn("lv_obj_set_style_text_font", ui_body)
        self.assertNotIn('SendLine("$H")', board)
        self.assertNotIn('SendLine("G90G0 X0Y0")', board)
        self.assertIn("Lang::Strings::MACHINE_ACTION_FAILED", board)
        self.assertIn("Lang::Strings::MACHINE_ACTION_SENT", board)
        self.assertIn("Lang::Strings::MACHINE_ACTION_STARTED", board)

    def test_machine_control_strings_exist_in_both_locales(self):
        keys = (
            "MACHINE_CONTROL",
            "MACHINE_PAUSE",
            "MACHINE_RESUME",
            "MACHINE_STOP",
            "MACHINE_REPEAT",
            "MACHINE_PEN_TEST",
            "MACHINE_CLOSE",
            "MACHINE_DRAWER_TITLE",
            "MACHINE_STATE_IDLE",
            "MACHINE_STATE_BUSY",
            "MACHINE_STATE_STREAMING",
            "MACHINE_STATE_PAUSED",
            "MACHINE_STATE_DONE",
            "MACHINE_STATE_ERROR",
            "MACHINE_STATE_ABORTED",
            "MACHINE_ACTION_FAILED",
            "MACHINE_ACTION_SENT",
            "MACHINE_ACTION_STARTED",
            "MACHINE_MANUAL_SECTION",
            "MACHINE_MANUAL_EXPAND",
            "MACHINE_MANUAL_COLLAPSE",
            "MACHINE_MAINT_EXPAND",
            "MACHINE_MAIN_PAGE",
            "MACHINE_REPROVISION",
            "MACHINE_REPROVISION_HINT",
            "MACHINE_PEN_UP",
            "MACHINE_PEN_DOWN",
            "MACHINE_JOG_XP",
            "MACHINE_JOG_XM",
            "MACHINE_JOG_YP",
            "MACHINE_JOG_YM",
            "MACHINE_JOG_STEP_1",
            "MACHINE_JOG_STEP_10",
            "MACHINE_PLOTTER_OFFLINE",
            "MACHINE_PLOTTER_NOT_READY",
            "MACHINE_HOME",
            "MACHINE_SET_ORIGIN",
            "MACHINE_UNLOCK",
            "MACHINE_MOTOR_OFF",
            "MACHINE_RESET",
            "MACHINE_STATE_MANUAL",
        )
        for locale in ("zh-CN", "en-US"):
            strings = json.loads(
                (ROOT / f"main/assets/locales/{locale}/language.json").read_text(
                    encoding="utf-8"
                )
            )["strings"]
            for key in keys:
                self.assertIn(key, strings, f"{locale} missing {key}")
                self.assertTrue(strings[key].strip(), f"{locale} {key} empty")
        # 展开文案点名点动（2026-08-20 可发现性决策）：不再用「高级调试」藏功能。
        zh = json.loads(
            (ROOT / "main/assets/locales/zh-CN/language.json").read_text(encoding="utf-8")
        )["strings"]
        en = json.loads(
            (ROOT / "main/assets/locales/en-US/language.json").read_text(encoding="utf-8")
        )["strings"]
        self.assertIn("点动", zh["MACHINE_MANUAL_EXPAND"])
        self.assertIn("Jog", en["MACHINE_MANUAL_EXPAND"])

    def test_manual_control_entry_is_gated_and_kx_aligned(self):
        """手动调试动作经 Job 统一入口：settled 态门控、独立任务执行、
        命令字面量逐字对齐奎享实测序列（2026-08-18 用户串口取证）。"""
        job_cc = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(encoding="utf-8")
        job_h = (ROOT / "main/boards/lichuang-dev/hutuji_job.h").read_text(encoding="utf-8")
        self.assertIn("std::string RequestManualControl(const std::string& action);", job_h)
        self.assertIn("static void ManualTaskEntry(void* arg);", job_h)
        self.assertIn("void ManualTask();", job_h)
        self.assertIn("bool QueryAndWaitFreshMachineState(uint32_t timeout_ms);", job_h)

        entry = job_cc[job_cc.index("std::string Job::RequestManualControl"):
                       job_cc.index("void Job::ManualTaskEntry")]
        # 白名单：11 个动作，未知 action 直接拒绝。
        for action in ("pen_up", "pen_down", "jog_x+", "jog_x-", "jog_y+", "jog_y-",
                       "home", "set_origin", "unlock", "motor_off", "reset",
                       "jog_step_1", "jog_step_10"):
            self.assertIn(f'"{action}"', entry)
        # 门控：仅 settled 态可用；busy 抢占失败即拒。
        self.assertIn('"idle"', entry)
        self.assertIn('"done"', entry)
        self.assertIn('"error"', entry)
        self.assertIn('"aborted"', entry)
        self.assertIn("busy_.exchange(true)", entry)
        self.assertIn("xTaskCreate(ManualTaskEntry", entry)
        # 1/10mm 步进切换只改 NVS，不占 busy、不进 ManualTask（否则点一下就
        # PERFORMANCE + 「手动控制中」，和「迈得开」相反）。
        self.assertIn("SetJogStepMm", entry)
        self.assertIn("GetJogStepMm", job_h)

        task = job_cc[job_cc.index("void Job::ManualTask()"):job_cc.index("std::string Job::StatusJson")]
        self.assertNotIn("jog_step_1", task)
        self.assertNotIn("jog_step_10", task)
        # 落笔对齐奎享：先 G92 Z0 声明，再 Z5 落笔；抬笔 Z0。
        self.assertLess(task.index('"G92 Z0"'), task.index('"G1G90 Z5.0F10000"'))
        self.assertIn('"G1G90 Z0.0F10000"', task)
        # 点动逐字对齐奎享 `$J=G21G91X1.0Y0.0Z0.0F8000.0` 格式，四方向参数化。
        self.assertIn('"$J=G21G91X', task)
        # 越界判定只信 `?` 之后的新鲜有限 MPos（fail closed），再交 core 判包线；
        # 缓存坐标直读已移除（2026-08-20 安全加固，无限位机器的唯一防线）。
        # 预算 6s 而非流式兜底的 2s：2026-08-20 HIL 实测 MQTT goodbye 后 WiFi
        # LOW_POWER(MAX_MODEM) 叠加 block-ack 拆链，Telnet 往返退化到 1.4~3.2s，
        # 2s 下连点第二下必假失败「点动前未取到新鲜坐标」；超时仍 fail closed。
        self.assertIn("kJogFreshStateTimeoutMs = 6000", job_cc)
        self.assertIn("QueryAndWaitFreshMachineState(kJogFreshStateTimeoutMs)", task)
        self.assertLess(
            task.index("QueryAndWaitFreshMachineState(kJogFreshStateTimeoutMs)"),
            task.index('"$J=G21G91X'),
        )
        self.assertIn("hutuji::DecideJog", task)
        self.assertIn("点动前未取到新鲜坐标", task)
        core = (ROOT / "main/boards/lichuang-dev/hutuji_recovery_core.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("kJogEnvelopeMaxXMm = 190.0f", core)
        self.assertIn("kJogEnvelopeMaxYMm = 190.0f", core)
        self.assertIn("kJogStepMm = 1.0f", core)
        self.assertIn("kJogStepMmFine = 1.0f", core)
        self.assertIn("kJogStepMmCoarse = 10.0f", core)
        self.assertIn("kJogStepMmDefault = kJogStepMmCoarse", core)
        self.assertIn("kMotorDisableLine", core)
        # 回原点和设原点：G1 归位（不触发换纸），G92 三轴声明对齐奎享。
        self.assertIn('"G1G90 X0Y0F8000"', task)
        self.assertIn('"G92 X0.0 Y0.0 Z0"', task)
        self.assertNotIn('"$H"', task)
        self.assertNotIn('SendLine("G90G0 X0Y0")', task)
        self.assertIn('"$X"', task)
        self.assertIn("kMotorDisableLine", task)
        self.assertNotIn('"$SLP"', task)
        self.assertIn("SendRealtime(0x18)", task)
        self.assertIn("GetResetBannerSequence()", task)
        # 收尾：释放 busy 并回 idle，结果走通知。
        self.assertIn('SetState("idle")', task)
        self.assertIn("busy_.store(false)", task)
        # 手动控制全程禁止触碰纸路机械命令（换纸职责边界不变）。
        for forbidden in ("M30", "ESP911", "ESP912", "ESP913", "M701", "M711"):
            self.assertNotIn(forbidden, task)

    def test_job_holds_performance_active_set_and_reassert_period(self):
        """射频 PERFORMANCE 持有决策与重申周期（2026-08-20 WiFi 省电抖动对策）。

        active 清单必须与显示层 ApplyMachineControlState 的 active 谓词同源——两侧
        任一改动不同步，会出现「屏上停止键可见但 WiFi 已掉回 MAX_MODEM」或反之。
        manual 明确不持有（整个 manual 态持有会让屏常亮）；重申周期取
        min(点动新鲜预算, 4000)，盖住实测最坏 BA 重建 3.2s 的空窗。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>

            int main() {
                using hutuji::JobHoldsPerformance;
                using hutuji::PerformanceReassertPeriodMs;

                // 与 lcd_display.cc active 谓词同源的 9 态全持有。
                for (const char* s : {"streaming", "paused", "previewing",
                                      "awaiting_confirmation", "downloading",
                                      "verifying", "reconnecting", "paper_change",
                                      "pen_test"}) {
                    assert(JobHoldsPerformance(s));
                }
                // settled 与 manual 不持有。
                for (const char* s : {"idle", "done", "error", "aborted", "manual"}) {
                    assert(!JobHoldsPerformance(s));
                }
                // 前缀/子串不得误判（手写比较器，非 strcmp）。
                assert(!JobHoldsPerformance("stream"));
                assert(!JobHoldsPerformance("streamingx"));
                assert(!JobHoldsPerformance("pause"));
                assert(!JobHoldsPerformance(""));

                // 重申周期 = min(预算, 4000)。
                assert(PerformanceReassertPeriodMs(6000) == 4000);
                assert(PerformanceReassertPeriodMs(4000) == 4000);
                assert(PerformanceReassertPeriodMs(2000) == 2000);
                assert(PerformanceReassertPeriodMs(1) == 1);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_perf_hold_test")

    def test_job_performance_hold_wiring(self):
        """PERFORMANCE 持有接线：SetState 进出 active 开/停持有、点动窗口短时持有、
        出窗口按 app 设备态回落（application.cc 零改动）。"""
        job_cc = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(
            encoding="utf-8"
        )
        job_h = (ROOT / "main/boards/lichuang-dev/hutuji_job.h").read_text(
            encoding="utf-8"
        )
        # SetState 尾按 JobHoldsPerformance 开/停持有。
        set_state = job_cc[
            job_cc.index("void Job::SetState(const char* state)"): job_cc.index(
                "void Job::StartPerformanceHold()"
            )
        ]
        self.assertIn("hutuji::JobHoldsPerformance", set_state)
        self.assertIn("StartPerformanceHold()", set_state)
        self.assertIn("StopPerformanceHold()", set_state)
        # 持有期幂等重申 + esp_timer 周期回调 + 立即重申一次。
        self.assertIn("esp_timer_create", job_cc)
        self.assertIn("esp_timer_start_periodic", job_cc)
        self.assertIn("ReassertPerformance();", job_cc)
        self.assertIn("SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE)", job_cc)
        # 出窗口回落尊重 app 音频通道（listening/speaking），不盲目降。
        self.assertIn("AppNeedsPerformance", job_cc)
        self.assertIn("kDeviceStateListening", job_cc)
        self.assertIn("kDeviceStateSpeaking", job_cc)
        self.assertIn("PowerSaveLevel::LOW_POWER", job_cc)
        # 点动新鲜坐标窗口短时持有：Start 在 `?` 之前、Stop 在 send_ok 之后。
        task = job_cc[
            job_cc.index("void Job::ManualTask()"): job_cc.index("std::string Job::StatusJson")
        ]
        self.assertLess(
            task.index("StartPerformanceHold()"),
            task.index("QueryAndWaitFreshMachineState(kJogFreshStateTimeoutMs)"),
        )
        self.assertIn("StopPerformanceHold();", task)
        self.assertLess(
            task.index('"$J=G21G91X'),
            task.index("StopPerformanceHold();"),
        )
        # 头文件声明 + esp_timer 成员。
        for decl in ("StartPerformanceHold", "StopPerformanceHold",
                     "ReassertPerformance", "PerformanceTimerThunk"):
            self.assertIn(decl, job_h)
        self.assertIn("esp_timer_handle_t performance_timer_", job_h)
        # 决策核在共享头里（host 可测），与显示层 active 谓词同源注释。
        core = (ROOT / "main/boards/lichuang-dev/hutuji_recovery_core.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("JobHoldsPerformance", core)
        self.assertIn("PerformanceReassertPeriodMs", core)

    def test_decide_jog_is_fail_closed_and_envelope_aligned(self):
        """点动判定 fail-closed：非有限输入一律 kStalePosition，包线恰 190/190。

        写字机无限位开关，新鲜 MPos + `DecideJog` 是点动唯一防线；包线与云端
        `protocol.md` §5 限幅同源（X≤190 / Y≤190mm），漂移即撞机风险。目标点
        恰贴包线合法（云端校验器同口径取等号），越出 1mm 步进即拒。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include <cassert>
            #include <cmath>
            #include <limits>
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            int main() {
                using namespace hutuji;
                // 包线内与恰贴包线：放行。
                assert(DecideJog(0.0f, 0.0f, kJogStepMm, 0.0f) == JogVerdict::kOk);
                assert(DecideJog(189.0f, 189.0f, kJogStepMm, kJogStepMm) == JogVerdict::kOk);
                assert(DecideJog(1.0f, 1.0f, -kJogStepMm, -kJogStepMm) == JogVerdict::kOk);
                // 越出包线：拒（四边各一例）。
                assert(DecideJog(190.0f, 100.0f, kJogStepMm, 0.0f) == JogVerdict::kOutOfBounds);
                assert(DecideJog(0.0f, 100.0f, -kJogStepMm, 0.0f) == JogVerdict::kOutOfBounds);
                assert(DecideJog(100.0f, 190.0f, 0.0f, kJogStepMm) == JogVerdict::kOutOfBounds);
                assert(DecideJog(100.0f, 0.0f, 0.0f, -kJogStepMm) == JogVerdict::kOutOfBounds);
                // 非有限输入 fail-closed 为「坐标不新鲜」，绝不放行运动。
                const float nan = std::nanf("");
                const float inf = std::numeric_limits<float>::infinity();
                assert(DecideJog(nan, 100.0f, kJogStepMm, 0.0f) == JogVerdict::kStalePosition);
                assert(DecideJog(100.0f, inf, 0.0f, kJogStepMm) == JogVerdict::kStalePosition);
                assert(DecideJog(100.0f, 100.0f, nan, 0.0f) == JogVerdict::kStalePosition);
                // 包线与步进常量逐值钉死，与云端 §5 / 奎享 `$J=` 序列同源。
                static_assert(kJogEnvelopeMaxXMm == 190.0f, "X envelope drifted from cloud S5");
                static_assert(kJogEnvelopeMaxYMm == 190.0f, "Y envelope drifted from cloud S5");
                static_assert(kJogStepMm == 1.0f, "fine jog step drifted from kx sequence");
                static_assert(kJogStepMmFine == 1.0f, "fine step must stay 1mm");
                static_assert(kJogStepMmCoarse == 10.0f, "coarse step must stay 10mm");
                static_assert(kJogStepMmDefault == kJogStepMmCoarse, "default step must be 10mm");
                assert(std::string(kMotorDisableLine) == "$MD");

                // 10mm 默认步进：恰贴包线放行，越出拒绝。
                assert(DecideJog(180.0f, 0.0f, kJogStepMmCoarse, 0.0f) == JogVerdict::kOk);
                assert(DecideJog(181.0f, 0.0f, kJogStepMmCoarse, 0.0f) == JogVerdict::kOutOfBounds);
                assert(ClampJogStepMm(1.0f) == kJogStepMmFine);
                assert(ClampJogStepMm(10.0f) == kJogStepMmCoarse);
                assert(ClampJogStepMm(7.0f) == kJogStepMmCoarse);
                assert(ClampJogStepMm(0.0f) == kJogStepMmFine);

                char hud[64];
                FormatMachineHud(hud, sizeof(hud), "Idle", 1.0f, 2.0f, 15.0f);
                assert(std::string(hud) == "Idle X1.0 Y2.0 Z15.0");
                FormatMachineHud(hud, sizeof(hud), "Sleep",
                                 std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
                assert(std::string(hud) == "Sleep X--- Y0.0 Z0.0");
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_decide_jog_test")

    def test_voice_manual_tool_whitelist(self):
        """语音手动控制白名单：维护动作（set_origin/unlock/motor_off/reset）不开放。

        hutuji.manual 把 RequestManualControl 暴露给云端 LLM；工具层必须先过
        IsVoiceAllowedAction 再进 RequestManualControl——RequestManualControl
        自身全量白名单含维护动作，语音误触 set_origin/unlock/motor_off/reset
        代价高（重写工作原点/失能/软复位），仅保留屏幕入口。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include <cassert>
            #include <string>
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            int main() {
                using namespace hutuji;
                // 运动/笔/步距/回原点：放行。
                for (const char* a : {"pen_up", "pen_down", "jog_x+", "jog_x-",
                                      "jog_y+", "jog_y-", "home", "jog_step_1",
                                      "jog_step_10"}) {
                    assert(IsVoiceAllowedAction(a));
                }
                // 维护动作与未知/大小写变体：一律拒绝。
                for (const char* a : {"set_origin", "unlock", "motor_off", "reset",
                                      "", "jog_z+", "JOG_X+", "jog_x"}) {
                    assert(!IsVoiceAllowedAction(a));
                }
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_voice_manual_test")
        # 双板工具都必须先过白名单再进 RequestManualControl（防 LLM 越权传维护动作）。
        for board_rel in (
            "main/boards/lichuang-dev/lichuang_dev_board.cc",
            "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc",
        ):
            body = (ROOT / board_rel).read_text(encoding="utf-8")
            self.assertIn('"hutuji.manual"', body)
            self.assertIn("IsVoiceAllowedAction", body)
            self.assertIn("RequestManualControl", body)

    def test_manual_control_wired_through_board_and_ui(self):
        """board 统一转发 action 字符串；UI 抽屉手动区按钮仅 settled 态可用。"""
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        # Pipe::Start 缺失会让一切机器控制静默失效（2026-08-18 实测回归：编辑误吞）。
        self.assertIn("hutuji::Pipe::GetInstance().Start();", board)
        self.assertIn("RequestManualControl", board)
        self.assertIn("ScheduleManualControl", board)
        lcd_cc = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        lcd_h = (ROOT / "main/display/lcd_display.h").read_text(encoding="utf-8")
        self.assertIn("std::function<void(const char* action)> on_manual", lcd_h)
        self.assertIn("machine_manual_", lcd_h)
        ui_body = lcd_cc[lcd_cc.index("void LcdDisplay::EnsureMachineControlUi()"):
                         lcd_cc.index("void LcdDisplay::ApplyMachineControlState()")]
        for label in ("MACHINE_PEN_UP", "MACHINE_PEN_DOWN", "MACHINE_HOME",
                      "MACHINE_SET_ORIGIN", "MACHINE_UNLOCK", "MACHINE_MOTOR_OFF",
                      "MACHINE_RESET", "MACHINE_MANUAL_SECTION"):
            self.assertIn(label, ui_body)
        self.assertIn("MACHINE_JOG", ui_body)
        state_body = lcd_cc[lcd_cc.index("void LcdDisplay::ApplyMachineControlState()"):
                            lcd_cc.index("void LcdDisplay::ConfigureMachineControls")]
        self.assertIn("machine_manual_buttons_", state_body)
        self.assertIn("settled", state_body)

    def test_visible_assistant_name_is_xiaopai_and_wake_word_is_real(self):
        """用户面统一小派；2026-08-20 声学模型已真换为 multinet mn7_cn 自定义词
        「xiao pai」，待机文案须如实告诉用户唤醒词是「小派」，且不得伪称
        「你好小派」（multinet 命令只有 xiao pai 两个音节，加「你好」唤不醒）。"""
        zh = json.loads(
            (ROOT / "main/assets/locales/zh-CN/language.json").read_text(encoding="utf-8")
        )["strings"]
        standby = zh["STANDBY"]
        self.assertNotIn("小智", standby)
        self.assertIn("小派", standby)
        self.assertNotIn("你好小派", standby)

        sdkconfig = (ROOT / "sdkconfig").read_text(encoding="utf-8")
        self.assertIn("CONFIG_USE_CUSTOM_WAKE_WORD=y", sdkconfig)
        self.assertIn('CONFIG_CUSTOM_WAKE_WORD="xiao pai"', sdkconfig)
        self.assertIn('CONFIG_CUSTOM_WAKE_WORD_DISPLAY="小派"', sdkconfig)
        self.assertIn("CONFIG_SR_MN_CN_MULTINET7_QUANT=y", sdkconfig)

        matrix = (
            ROOT / "main/boards/waveshare/esp32-s3-rgb-matrix/rgb_matrix_display.cc"
        ).read_text(encoding="utf-8")
        self.assertIn('lv_label_set_text(message_label_, "hi 小派")', matrix)
        self.assertNotIn('lv_label_set_text(message_label_, "hi 小智")', matrix)

    def test_activation_relay_wiring_and_redaction(self):
        """激活码中转接线（2026-08-20 用户决策，京东云中转）：
        中继源进 waveshare 编译面、application.cc 钩子带板型守卫、
        日志绝不出现激活码/请求体（脱敏硬约定）。"""
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("hutuji_activation_relay.cc", cmake)

        app_cc = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn("hutuji_activation_relay.h", app_cc)
        self.assertIn("hutuji::ReportActivationCode(ota_->GetActivationCode())", app_cc)
        # 钩子必须带板型守卫，其余板型上游行为不变。
        hook = app_cc[app_cc.index("hutuji::ReportActivationCode(ota_->GetActivationCode())"):]
        guard_region = app_cc[:app_cc.index("hutuji::ReportActivationCode(ota_->GetActivationCode())")]
        self.assertIn("#ifdef CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_5",
                      guard_region[-800:])
        self.assertIn("#endif", hook[:200])

        relay = (ROOT / "main/boards/lichuang-dev/hutuji_activation_relay.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("https://hutuji.donglicao.com/register", relay)
        self.assertIn('cJSON_AddStringToObject(root, "mac"', relay)
        self.assertIn('cJSON_AddStringToObject(root, "code"', relay)
        self.assertIn('SetHeader("Content-Type", "application/json")', relay)
        self.assertIn('http->Open("POST", kRelayUrl)', relay)
        # 一次性异步任务：不阻塞激活循环；失败路径都要释放 payload 并自删。
        self.assertIn("xTaskCreate(RelayTask", relay)
        self.assertGreaterEqual(relay.count("vTaskDelete(nullptr)"), 4)
        # 脱敏：日志调用不得直接格式化激活码或请求体变量。
        for line in relay.splitlines():
            if "ESP_LOG" in line:
                self.assertNotIn("payload->code", line)
                self.assertNotIn("body", line)

    def test_discover_miss_retries_cache_instead_of_skipping_plotter(self):
        """刷机/复位后 Telnet 槽位忙时不得扫网跳过缓存 IP（2026-08-22）。

        社区对照：FluidNC #189（硬复位客户端不发 TCP 关闭，服务端仍占唯一会话）；
        上游 Grbl_Esp32 / ESP3D 槽位满则拒绝新连接。我方旧路径在 ClosedBeforeBanner
        后扫 /24 并 `continue` 掉缓存 IP，把 ~19s keepalive 放大成分钟级。
        决策：槽位忙只 1s 重试缓存、不扫网、不指数退避；扫网只跳过已验明非 Grbl
        的缓存；扫描超时 ≥200ms 盖住首包 modem-sleep。
        """
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")

        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>

            int main() {
                using hutuji::DiscoverMiss;
                using hutuji::DiscoverRetryDelayMs;
                using hutuji::RetryCachedIpAfterScan;
                using hutuji::ShouldAdvanceDiscoverBackoff;
                using hutuji::ShouldScanSubnet;
                using hutuji::SkipCachedIpDuringScan;
                using hutuji::kPipeCachedTimeoutMs;
                using hutuji::kPipeScanTimeoutMs;
                using hutuji::kSlotBusyRetryDelayMs;
                using hutuji::kWaitingIpRetryDelayMs;

                static_assert(kPipeScanTimeoutMs >= 200, "scan timeout shorter than first-hop PS delay");
                static_assert(kPipeCachedTimeoutMs == 2000, "cached connect timeout drifted");
                static_assert(kSlotBusyRetryDelayMs == 1000, "slot-busy delay drifted");
                static_assert(kWaitingIpRetryDelayMs == 1000, "waiting-ip delay drifted");

                // 只有「连上了但不是写字机」才在扫网时跳过该 IP。
                assert(!SkipCachedIpDuringScan(DiscoverMiss::WaitingIp));
                assert(!SkipCachedIpDuringScan(DiscoverMiss::SlotBusy));
                assert(!SkipCachedIpDuringScan(DiscoverMiss::CacheUnreachable));
                assert(SkipCachedIpDuringScan(DiscoverMiss::CacheNotGrbl));
                assert(!SkipCachedIpDuringScan(DiscoverMiss::ScanEmpty));

                // 槽位忙 / 还没 IP：禁止扫网（会跳过或漏掉真机）。
                assert(!ShouldScanSubnet(DiscoverMiss::WaitingIp));
                assert(!ShouldScanSubnet(DiscoverMiss::SlotBusy));
                assert(ShouldScanSubnet(DiscoverMiss::CacheUnreachable));
                assert(ShouldScanSubnet(DiscoverMiss::CacheNotGrbl));
                assert(ShouldScanSubnet(DiscoverMiss::ScanEmpty));

                // 扫网漏检时，对「TCP 没连上」的缓存再用 2s 打一次；非 Grbl 不打。
                assert(RetryCachedIpAfterScan(DiscoverMiss::CacheUnreachable, false));
                assert(!RetryCachedIpAfterScan(DiscoverMiss::CacheUnreachable, true));
                assert(!RetryCachedIpAfterScan(DiscoverMiss::CacheNotGrbl, false));
                assert(!RetryCachedIpAfterScan(DiscoverMiss::SlotBusy, false));
                assert(!RetryCachedIpAfterScan(DiscoverMiss::ScanEmpty, false));

                // 槽位忙 / 等 IP：固定 1s，不走 1→2→4→8→16→30。
                assert(DiscoverRetryDelayMs(DiscoverMiss::SlotBusy, 16000) == 1000);
                assert(DiscoverRetryDelayMs(DiscoverMiss::WaitingIp, 30000) == 1000);
                assert(DiscoverRetryDelayMs(DiscoverMiss::ScanEmpty, 8000) == 8000);
                assert(DiscoverRetryDelayMs(DiscoverMiss::CacheUnreachable, 4000) == 4000);
                assert(!ShouldAdvanceDiscoverBackoff(DiscoverMiss::SlotBusy));
                assert(!ShouldAdvanceDiscoverBackoff(DiscoverMiss::WaitingIp));
                assert(ShouldAdvanceDiscoverBackoff(DiscoverMiss::ScanEmpty));
                assert(ShouldAdvanceDiscoverBackoff(DiscoverMiss::CacheUnreachable));
                assert(ShouldAdvanceDiscoverBackoff(DiscoverMiss::CacheNotGrbl));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_discover_miss_test")

        pipe = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("#define HUTUJI_PIPE_SCAN_TIMEOUT_MS 50", pipe)
        self.assertIn("kPipeScanTimeoutMs", pipe)
        self.assertIn("SkipCachedIpDuringScan", pipe)
        self.assertIn("ShouldScanSubnet", pipe)
        self.assertIn("RetryCachedIpAfterScan", pipe)
        self.assertIn("DiscoverRetryDelayMs", pipe)
        self.assertIn("ShouldAdvanceDiscoverBackoff", pipe)
        connect = pipe[
            pipe.index("bool Pipe::ConnectOnce()") : pipe.index("void Pipe::CloseSocket()")
        ]
        self.assertIn("SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE)", connect)
        close = pipe[
            pipe.index("void Pipe::CloseSocketLocked()") : pipe.index(
                "void Pipe::ShutdownSocket"
            )
        ]
        self.assertIn("shutdown(sock_, SHUT_RDWR)", close)

    def test_discover_backoff_fast_retry_prefix(self):
        """写字机重连退避前段收敛（2026-08-23 断联取证）：前 5 次真失败保持 1s
        （WiFi 瞬断/对端重启场景秒级恢复），之后翻倍至 30s 封顶；SlotBusy/
        WaitingIp 不计入推进（由 ShouldAdvanceDiscoverBackoff 既有语义保证）。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstdint>

            int main() {
                using namespace hutuji;
                static_assert(kDiscoverFastRetryAttempts == 5);
                // 前段不涨：attempt 1..5 都保持 1000ms
                uint32_t backoff = 1000;
                for (int attempt = 1; attempt <= 5; ++attempt) {
                    backoff = NextDiscoverBackoffMs(attempt, backoff, 30000);
                    assert(backoff == 1000);
                }
                // 第 6 次起翻倍：1000 → 2000 → 4000 → ... → 30000 封顶
                backoff = NextDiscoverBackoffMs(6, backoff, 30000);
                assert(backoff == 2000);
                backoff = NextDiscoverBackoffMs(7, backoff, 30000);
                assert(backoff == 4000);
                backoff = NextDiscoverBackoffMs(8, backoff, 30000);
                assert(backoff == 8000);
                backoff = NextDiscoverBackoffMs(9, backoff, 30000);
                assert(backoff == 16000);
                backoff = NextDiscoverBackoffMs(10, backoff, 30000);
                assert(backoff == 30000);
                backoff = NextDiscoverBackoffMs(11, backoff, 30000);
                assert(backoff == 30000);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_backoff_fast_retry_test")

        pipe = (ROOT / "main/boards/lichuang-dev/hutuji_pipe.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("NextDiscoverBackoffMs(++miss_attempts", pipe)
        self.assertIn("miss_attempts = 0", pipe, "连上后必须清零快速重试计数")

    def test_power_save_gate_and_balanced_floor_both_boards(self):
        """双板同口径（2026-08-23 卡顿/断联取证，平衡档）：
        ①出图活跃期拒绝一切非 PERFORMANCE 回落（门控 HoldsPerformanceForRadio）；
        ②稳态 LOW_POWER(MAX_MODEM) 映射为 BALANCED(MIN_MODEM)——MAX_MODEM 长睡眠
        是写字机断联（reason 3 / errno=113）与慢发现（首包 ~200ms）的头号嫌疑。"""
        for rel in (
            "main/boards/lichuang-dev/lichuang_dev_board.cc",
            "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc",
        ):
            board = (ROOT / rel).read_text(encoding="utf-8")
            self.assertIn("SetPowerSaveLevel(PowerSaveLevel level) override", board, rel)
            self.assertIn("level = PowerSaveLevel::BALANCED", board, rel)
            self.assertIn("HoldsPerformanceForRadio", board, rel)
            self.assertIn("level != PowerSaveLevel::PERFORMANCE", board, rel)

    def test_parse_paper_status_fields_full_and_partial(self):
        """[ESP901] 遥测三字段解析（2026-08-24 P1-1）：协议 §4 status 分级承诺空闲期
        Paper/MotorEn/PanelHold 遥测，实现缺。解析必须与 Changing 同一守卫——
        「Paper= 与 Changing= 同现」才认行，防止把其他含 Paper= 的日志误当遥测；
        部分字段缺席保持 Unknown，不得整行作废。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>
            #include <cstring>

            int main() {
                using namespace hutuji;
                PaperPresentState paper = PaperPresentState::Unknown;
                MotorEnState motor = MotorEnState::Unknown;
                PanelHoldState panel = PanelHoldState::Unknown;

                // 全字段：Paper=OK / MotorEn=On / PanelHold=Off
                assert(ParsePaperStatusFields(
                    "Paper=OK MotorEn=On PanelHold=Off Changing=Off", paper, motor, panel));
                assert(paper == PaperPresentState::Yes);
                assert(motor == MotorEnState::On);
                assert(panel == PanelHoldState::Off);

                // 全字段另一组值：Paper=No / MotorEn=Off / PanelHold=On
                paper = PaperPresentState::Unknown;
                motor = MotorEnState::Unknown;
                panel = PanelHoldState::Unknown;
                assert(ParsePaperStatusFields(
                    "Paper=No MotorEn=Off PanelHold=On Changing=On", paper, motor, panel));
                assert(paper == PaperPresentState::No);
                assert(motor == MotorEnState::Off);
                assert(panel == PanelHoldState::On);

                // 守卫：缺 Changing= 的行不认（字段必须保持 Unknown 且返回 false）
                paper = PaperPresentState::Unknown;
                motor = MotorEnState::Unknown;
                panel = PanelHoldState::Unknown;
                assert(!ParsePaperStatusFields(
                    "Paper=OK MotorEn=On PanelHold=Off", paper, motor, panel));
                assert(paper == PaperPresentState::Unknown);
                assert(motor == MotorEnState::Unknown);
                assert(panel == PanelHoldState::Unknown);

                // 部分字段：只有 Paper=，其余保持 Unknown，行仍算遥测行
                paper = PaperPresentState::Unknown;
                motor = MotorEnState::Unknown;
                panel = PanelHoldState::Unknown;
                assert(ParsePaperStatusFields("Paper=OK Changing=Off", paper, motor, panel));
                assert(paper == PaperPresentState::Yes);
                assert(motor == MotorEnState::Unknown);
                assert(panel == PanelHoldState::Unknown);

                // 命名函数供 StatusJson 序列化
                assert(std::strcmp(PaperPresentStateName(PaperPresentState::Yes), "yes") == 0);
                assert(std::strcmp(PaperPresentStateName(PaperPresentState::No), "no") == 0);
                assert(std::strcmp(PaperPresentStateName(PaperPresentState::Unknown), "unknown") == 0);
                assert(std::strcmp(MotorEnStateName(MotorEnState::On), "on") == 0);
                assert(std::strcmp(PanelHoldStateName(PanelHoldState::Off), "off") == 0);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_paper_status_fields_test")



    def test_status_json_never_sends_commands(self):
        """StatusJson 只读禁令（2026-08-24 实锤回归）：[ESP901] 是普通命令会吃 ok，
        StatusJson 发而不消费会在响应队列留孤儿 ok——下个窗口化出图把队列应答当
        在途凭据，计数凭空多一格（提前多发一行、ok 配对错位）。遥测刷新只许走
        Preview() 的「发 + WaitResponse 消费」路径。"""
        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(
            encoding="utf-8"
        )
        start = job.index("std::string Job::StatusJson() const {")
        end = job.index("void Job::PreviewTaskEntry", start)
        body = job[start:end]
        self.assertNotIn("SendLine(", body, "StatusJson 体内不得发任何普通命令")
        self.assertIn("SendRealtime('?')", body, "实时 ? 旁路保留")
        preview = job[job.index("void Job::Preview() {"):]
        self.assertIn('SendLine("[ESP901]")', preview, "遥测刷新应落在预览任务内")
        self.assertIn("WaitResponse(kPaperStatusTimeoutMs", preview, "刷新必须消费 ok")

    def test_duplicate_preview_reentry_idempotent(self):
        """预览重入幂等（2026-08-24 P1-3 配套 + fixup）：服务端链式调用在生成完成点
        即发，云端第二步以同 url/preview_url 重入 StartDraw——第一步还在下载 PNG
        （state=previewing）时第二步就会到。判据必须是「已在预览流程中」
        （previewing 或 awaiting_confirmation），不是 awaiting_confirmation_ 标志
        （它要等 PNG 下载完才置 true，用它做主路径判据等于没修）。同参数重入回
        previewing；参数不同或不在预览流程（如 streaming）才判 busy。"""
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_recovery_core.h"

            #include <cassert>

            int main() {
                using namespace hutuji;
                static_assert(IsDuplicatePreviewReentry(true, true),
                              "等确认 + 同参数 = 幂等重入");
                static_assert(!IsDuplicatePreviewReentry(true, false),
                              "等确认但参数不同 = busy");
                static_assert(!IsDuplicatePreviewReentry(false, true),
                              "未在等确认（出图中）= busy");
                static_assert(!IsDuplicatePreviewReentry(false, false),
                              "未在等确认且参数不同 = busy");
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, stem="hutuji_preview_reentry_test")
        job = (ROOT / "main/boards/lichuang-dev/hutuji_job.cc").read_text(
            encoding="utf-8"
        )
        start = job.index("std::string Job::StartDraw(")
        end = job.index("std::string Job::RequestConfirm", start)
        body = job[start:end]
        self.assertIn("IsDuplicatePreviewReentry", body, "StartDraw 必须走幂等重入判定")
        self.assertIn(
            'state == "previewing" || state == "awaiting_confirmation"',
            body,
            "谓词必须是「已在预览流程中」——awaiting_confirmation_ 要等 PNG 下载完才置 true，主路径不成立",
        )
        self.assertIn('return JsonString("previewing")', body, "幂等重入须回 previewing")


if __name__ == "__main__":
    unittest.main()


