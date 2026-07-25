# AGENTS.md

## Project

XiaoZhi is an ESP-IDF C/C++ voice-assistant firmware supporting many chips, boards, displays, audio devices, and network transports. A build selects exactly one board implementation.

Use ESP-IDF v6.0.2 when possible. IDF 5.5.x is retained only for documented legacy boards.

## Architecture

- `main/application.*`: main event loop, protocol lifecycle, and high-level behavior.
- `main/device_state_machine.*`: legal runtime state transitions.
- `main/boards/common/`: board interfaces and reusable hardware/network helpers.
- `main/boards/**/`: board-specific pins, initialization, and build variants.
- `main/audio/`: codecs, audio tasks, engines, wake words, and queues.
- `main/protocols/`: transport-neutral API plus WebSocket and MQTT/UDP.
- `main/display/` and `main/led/`: reusable UI implementations.
- `main/mcp_server.*`: common device-side MCP tools and dispatch.
- `main/Kconfig.projbuild`: board and feature configuration.
- `main/CMakeLists.txt`: source, board, locale, font, and asset selection.
- `scripts/release.py`: canonical board/variant build entry point.

Read the closest existing implementation before adding a new one. Prefer the narrowest owning layer; do not put board-specific behavior into core modules.

## Required Rules

- Preserve unrelated worktree changes and keep patches focused.
- A build must export exactly one board factory through `DECLARE_BOARD(...)`.
- Never alter an existing board's pins to support different hardware. Add a uniquely named board or release variant; board identity affects OTA compatibility.
- Core code depends on `Board` interfaces, never a concrete board class or board `config.h`.
- Treat camera, backlight, display, LED, battery, and similar capabilities as optional.
- Change runtime state through `Application::SetDeviceState()` and the state machine.
- Callbacks may run outside the main task. Schedule application mutations with `Application::Schedule()` or event bits.
- Do not block the main event loop or audio tasks. Avoid unbounded queues and repeated large allocations in audio paths.
- Keep shared message semantics in `Protocol`; verify both transports when changing its contract.
- Validate network input and preserve `cJSON` ownership. NVS keys are persistent API and require migration when changed.
- Guard target-specific features with Kconfig/component rules. Do not assume every target has PSRAM or S3/P4 resources.
- Do not manually edit generated/vendor output: `build/`, `releases/`, `managed_components/`, `components/`, `sdkconfig*`, `main/assets/lang_config.h`, or generated mmap headers.
- Format only touched C/C++ files with the repository `.clang-format`; avoid unrelated mass formatting.

## Boards and Configuration

Board selection is a coupled chain:

`config.json` -> `scripts/release.py` -> `main/Kconfig.projbuild` -> `main/CMakeLists.txt` -> board source and `config.h`.

When adding a board or variant, update every relevant link in that chain. Include a unique board identity, correct chip target, flash/partition settings, exactly one `DECLARE_BOARD`, and board documentation. Follow `docs/custom-board.md`.

## Commands

Source the intended ESP-IDF environment first:

```sh
source /path/to/esp-idf/export.sh
idf.py --version
```

```sh
# Discover exact board and variant names
python3 scripts/release.py --list-boards

# Canonical variant build
python3 scripts/release.py <board-directory> --name <variant-name>

# Host-side release tests
python3 -m unittest discover -s scripts/tests -v

# Format/check touched files
clang-format -i <files>
clang-format --dry-run -Werror <files>
```

The release script changes local `sdkconfig` and build state. Do not assume the build directory still represents a previous target.

## Validation

- Board-only change: build affected variants and smoke-test changed hardware.
- Core, common-board, audio, protocol, display, dependency, Kconfig, or CMake change: run host tests and build representative affected chip/network paths.
- Protocol changes: verify WebSocket and MQTT/UDP when shared behavior changes.
- Audio changes: verify capture, playback, wake/VAD, interruption, reconnect, and applicable AEC modes.
- UI/assets changes: verify applicable no-display/OLED/LVGL paths and partition size.
- Always report what was tested and what still needs physical hardware. A successful build is not hardware validation.

## Authoritative Documentation

- Overview and SDK policy: `README.md`
- SDK compatibility: `docs/esp-idf-6-migration.md`
- Board guide: `docs/custom-board.md`
- Audio design: `main/audio/README.md`
- Code style: `docs/code_style.md`
- Protocols: `docs/websocket.md`, `docs/mqtt-udp.md`, `docs/mcp-protocol.md`
- CI matrix: `.github/workflows/build.yml`

Keep detailed or fast-changing information in those files, not here. Add a nested `AGENTS.md` only when a subsystem needs specialized instructions.

---

## hutuji（写字机哑管道）专节

本 fork 板型 `lichuang-dev` 承载 hutuji **方案 E：WiFi Telnet** 哑管道。枢纽真值在 `D:/Users/hutuji`，冲突以枢纽为准。

### 动手前必读

1. `D:/Users/hutuji/docs/agent-handoff.md`
2. `D:/Users/hutuji/docs/agent-anti-drift.md`
3. `D:/Users/hutuji/docs/protocol.md` v0.5（流控 / 单写者 / abort / §9）
4. `D:/Users/hutuji/firmware/m1-usb-pipe.md`（分支名含 usb 是历史名，链路已是 Telnet）
5. `D:/Users/hutuji/docs/worktree-inventory.md` + 本仓 `git status`

### 硬约束

- **哑管道**：不在 S3 生成/编辑/预览 G-code；只下载 + 校验 + 严格逐行转发。
- **板级行为不进核心**：工具挂在板级 `InitializeTools()`；参照既有 board MCP 工具模式。
- **出图中 status 只发 `?`**；禁止发 `[ESP901]` / `M704`（会吃 `ok`）。
- **TCP keepalive** 必须设 `KEEPIDLE/KEEPINTVL/KEEPCNT`（见 protocol §1.2）；只开 `SO_KEEPALIVE` 无效。
- **禁止**重开 USB Host / CH9350；禁止合并双芯片固件进单一镜像。
- 出货前 `HUTUJI_PIPE_HOST` 必须是写字机固定 IP，禁止带开发占位值放行。
- 保留无关工作树改动；M2（下载/CRC/授权门/流控/abort）未完成前不得宣称联调完成。

### 构建

```sh
python3 scripts/release.py lichuang-dev
```

说明同步回枢纽 `D:/Users/hutuji/firmware/`。硬件结论必须带设备编号与固件 commit。
