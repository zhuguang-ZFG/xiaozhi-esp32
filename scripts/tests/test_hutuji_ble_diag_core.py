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


class HutujiBleDiagCoreTest(unittest.TestCase):
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

    def test_phase_a_payload_is_six_bytes_and_little_endian(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <array>
            #include <cassert>
            #include <cstdint>

            int main() {
                using namespace hutuji::ble_diag;
                static_assert(kPhaseASchemaVersion == 0x10);
                static_assert(kPhaseAPayloadBytes == 6);
                const auto payload = MakePhaseAPayload(Health::Degraded, 0x03, 0x1234, false);
                const auto bytes = SerializePhaseAPayload(payload);
                static_assert(sizeof(bytes) == kPhaseAPayloadBytes);
                const std::array<uint8_t, 6> expected = {0x10, 0x02, 0x03, 0x34, 0x12, 0x00};
                assert(bytes == expected);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_payload_test")

    def test_phase_a_sanitizes_reserved_bits_invalid_health_and_stale(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cassert>
            #include <cstdint>

            int main() {
                using namespace hutuji::ble_diag;
                const auto invalid = MakePhaseAPayload(static_cast<Health>(0xff), 0xff, 0xffff, true);
                const auto bytes = SerializePhaseAPayload(invalid);
                assert(bytes[0] == 0x10);
                assert(bytes[1] == static_cast<uint8_t>(Health::Unknown));
                assert(bytes[2] == 0x03);
                assert(bytes[3] == 0xff && bytes[4] == 0xff);
                assert(bytes[5] == 1);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_sanitize_test")

    def test_phase_a_sequence_wraps_without_touching_adjacent_fields(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cassert>

            int main() {
                using namespace hutuji::ble_diag;
                auto sequence = uint16_t{0xffff};
                sequence = NextSnapshotSequence(sequence);
                assert(sequence == 0);
                const auto bytes = SerializePhaseAPayload(
                    MakePhaseAPayload(Health::Fault, 0x02, sequence, true));
                assert(bytes[1] == static_cast<uint8_t>(Health::Fault));
                assert(bytes[2] == 0x02);
                assert(bytes[3] == 0 && bytes[4] == 0);
                assert(bytes[5] == 1);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_sequence_test")

    def test_phase_a_stale_consumption_is_unknown(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cassert>

            int main() {
                using namespace hutuji::ble_diag;
                assert(HealthForConsumer(Health::Nominal, false) == Health::Nominal);
                assert(HealthForConsumer(Health::Fault, false) == Health::Fault);
                assert(HealthForConsumer(Health::Fault, true) == Health::Unknown);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_stale_test")

    def test_service_uuid_is_bluetooth_wire_order_not_string_order(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cstdint>
            #include <iostream>

            int main() {
                using namespace hutuji::ble_diag;
                // d3e6a7b0-7c22-4f61-9b18-2e4d5f6a7001 逐字节反序即空中线序。
                const uint8_t expected[16] = {0x01, 0x70, 0x6a, 0x5f, 0x4d, 0x2e, 0x18, 0x9b,
                                              0x61, 0x4f, 0x22, 0x7c, 0xb0, 0xa7, 0xe6, 0xd3};
                if (kServiceUuid128.size() != 16u) {
                    std::cerr << "uuid size" << std::endl;
                    return 1;
                }
                for (std::size_t i = 0; i < 16u; ++i) {
                    if (kServiceUuid128[i] != expected[i]) {
                        std::cerr << "uuid byte " << i << std::endl;
                        return 1;
                    }
                }
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_uuid_test")

    def test_advertising_ad_structure_is_24_bytes_within_legacy_budget(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cstdint>
            #include <iostream>

            int main() {
                using namespace hutuji::ble_diag;
                static_assert(kAdvertisingBytes == 24);
                static_assert(kAdvertisingBytes <= kLegacyAdvBudgetBytes);
                static_assert(kLegacyAdvBudgetBytes == 31);
                const auto ad = SerializeAdvertisingData(
                    MakePhaseAPayload(Health::Nominal, 0x01, 0x0102, false));
                if (ad.size() != kAdvertisingBytes) {
                    std::cerr << "ad size" << std::endl;
                    return 1;
                }
                // length 覆盖 type+UUID+payload，不含自身。
                if (ad[0] != 23 || ad[1] != 0x21) {
                    std::cerr << "ad header" << std::endl;
                    return 1;
                }
                for (std::size_t i = 0; i < kServiceUuid128.size(); ++i) {
                    if (ad[2 + i] != kServiceUuid128[i]) {
                        std::cerr << "ad uuid " << i << std::endl;
                        return 1;
                    }
                }
                const uint8_t expected_payload[6] = {0x10, 0x01, 0x01, 0x02, 0x01, 0x00};
                for (std::size_t i = 0; i < 6u; ++i) {
                    if (ad[18 + i] != expected_payload[i]) {
                        std::cerr << "ad payload " << i << std::endl;
                        return 1;
                    }
                }
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_ad_test")

    def test_link_flags_only_expose_wifi_and_telnet_bits(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cassert>

            int main() {
                using namespace hutuji::ble_diag;
                assert(MakeLinkFlags(false, false) == 0x00);
                assert(MakeLinkFlags(true, false) == 0x01);
                assert(MakeLinkFlags(false, true) == 0x02);
                assert(MakeLinkFlags(true, true) == 0x03);
                static_assert((MakeLinkFlags(true, true) & ~kKnownLinkFlags) == 0);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_flags_test")

    def test_derive_health_ranks_stale_over_fault_over_degraded(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cassert>

            int main() {
                using namespace hutuji::ble_diag;
                // 链路齐全且无故障才是 nominal。
                assert(DeriveHealth(true, true, false, false) == Health::Nominal);
                // WiFi 或 Telnet 任一不可用即 degraded，不得报 nominal。
                assert(DeriveHealth(false, true, false, false) == Health::Degraded);
                assert(DeriveHealth(true, false, false, false) == Health::Degraded);
                // 当前故障压过链路态。
                assert(DeriveHealth(true, true, true, false) == Health::Fault);
                assert(DeriveHealth(false, false, true, false) == Health::Fault);
                // stale 压过一切，只能报 unknown。
                assert(DeriveHealth(true, true, false, true) == Health::Unknown);
                assert(DeriveHealth(true, true, true, true) == Health::Unknown);
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_health_test")

    def test_snapshot_is_stale_past_five_seconds_or_without_clock(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no supported host C++ compiler found")
        source = textwrap.dedent(
            r"""
            #include "main/boards/lichuang-dev/hutuji_ble_diag_core.h"
            #include <cassert>

            int main() {
                using namespace hutuji::ble_diag;
                static_assert(kSnapshotStaleAfterSeconds == 5);
                assert(!IsSnapshotStale(true, 0));
                assert(!IsSnapshotStale(true, 5000));
                // 契约是 age_s > 5，5000ms 仍新鲜，5001ms 起判 stale。
                assert(IsSnapshotStale(true, 5001));
                // 取不到单调时钟只能判 stale，不得猜新鲜。
                assert(IsSnapshotStale(false, 0));
                return 0;
            }
            """
        )
        self._compile_and_run(compiler, source, "hutuji_ble_diag_freshness_test")

    def test_adapter_latches_fail_closed_and_checks_npl_timers(self):
        adapter = (
            ROOT / "main/boards/lichuang-dev/hutuji_ble_diag.cc"
        ).read_text(encoding="utf-8")
        self.assertIn("bool disabled = false;", adapter)
        self.assertIn("g_state.disabled = true;", adapter)
        self.assertIn("if (g_state.disabled)", adapter)
        self.assertIn("snapshot callout init failed", adapter)
        self.assertIn("rotation callout init failed", adapter)
        self.assertIn("snapshot timer arm failed", adapter)
        self.assertIn("rotation timer arm failed", adapter)

    def test_advertising_interval_is_shorter_than_snapshot_refresh(self):
        adapter = (
            ROOT / "main/boards/lichuang-dev/hutuji_ble_diag.cc"
        ).read_text(encoding="utf-8")

        def constant(name):
            prefix = f"constexpr uint32_t {name} = "
            line = next(line for line in adapter.splitlines() if line.startswith(prefix))
            return int(line.removeprefix(prefix).removesuffix(";"))

        self.assertLess(constant("kAdvIntervalMs"), constant("kSnapshotIntervalMs"))

    def test_snapshot_refresh_restarts_legacy_advertising(self):
        adapter = (
            ROOT / "main/boards/lichuang-dev/hutuji_ble_diag.cc"
        ).read_text(encoding="utf-8")
        refresh = adapter.split("int RefreshAdvertising()", 1)[1].split(
            "void OnSnapshotTimer", 1
        )[0]
        stop = refresh.index("ble_gap_adv_stop()")
        publish = refresh.index("PublishSnapshot()")
        start = refresh.index("StartAdvertising()")
        self.assertLess(stop, publish)
        self.assertLess(publish, start)

    def test_board_integration_is_default_off_and_has_no_gatt_surface(self):
        adapter = (
            ROOT / "main/boards/lichuang-dev/hutuji_ble_diag.cc"
        ).read_text(encoding="utf-8")
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        waveshare = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc"
        ).read_text(encoding="utf-8")
        lichuang = (
            ROOT / "main/boards/lichuang-dev/lichuang_dev_board.cc"
        ).read_text(encoding="utf-8")

        config_block = kconfig.split("config HUTUJI_BLE_DIAGNOSTICS", 1)[1].split(
            "config AUDIO_DEBUG_UDP_SERVER", 1
        )[0]
        self.assertIn("default n", config_block)
        self.assertIn("depends on BT_ENABLED && BT_NIMBLE_ENABLED", config_block)
        self.assertIn("depends on !USE_ESP_BLUFI_WIFI_PROVISIONING", config_block)
        self.assertIn("BLE_GAP_CONN_MODE_NON", adapter)
        self.assertIn("BLE_GAP_DISC_MODE_NON", adapter)
        self.assertIn("ble_hs_id_gen_rnd(1, &addr)", adapter)
        self.assertNotIn("ble_gatts_", adapter)
        self.assertNotIn("ble_svc_gatt", adapter)
        self.assertNotIn("ble_gap_adv_rsp_set_data", adapter)
        self.assertIn("hutuji_ble_diag.cc", cmake)
        self.assertIn("hutuji::ble_diag::Start();", waveshare)
        self.assertIn("hutuji::ble_diag::Start();", lichuang)


if __name__ == "__main__":
    unittest.main()
