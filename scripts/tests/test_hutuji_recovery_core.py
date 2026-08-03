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
