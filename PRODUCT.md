# Product

<!-- impeccable:product-schema 1 -->

## Users

Family users, including children, and makers/debug operators. The default surface must be safe and legible for children, while advanced machine controls remain reachable for setup, calibration, and troubleshooting.

## Product Purpose

hutuji turns a spoken or conversational idea into a physical drawing on paper. The touchscreen closes the loop that voice alone cannot make safe: preview the generated drawing, confirm or cancel it, observe machine state, and control pause, resume, stop, repeat, pen test, and maintenance actions.

## Positioning

Unlike a generic voice assistant, this product finishes a physical workflow: cloud generation produces a validated G-code artifact, the ESP32-S3 downloads and forwards it over the Telnet pipe, and the plotter draws and changes paper. The device UI is the local trust and safety surface for that physical action.

## Operating Context

- Runs on the Waveshare ESP32-S3 Touch LCD 3.5 board, used as a 480×320 landscape LVGL touchscreen.
- Voice interaction and touch interaction are both first-class; touch is not merely a fallback.
- Typical use happens near the drawing machine, where hands may be occupied and errors have physical consequences.
- Debug operators need pen up/down, X/Y jog, home, origin, unlock, motor off, and reset controls without making those actions prominent to children.

## Capabilities and Constraints

- Preserve the existing hutuji MCP/device workflow and callbacks: preview, confirm, abort, pause, resume, repeat, pen test, status, and manual control.
- The touchscreen must show a clear drawing preview confirmation before motion starts.
- Machine controls must reflect the current job state and disable invalid actions rather than pretending every command is always available.
- Manual/debug controls should be grouped behind an advanced section in the control drawer.
- The implementation must stay within the Waveshare board and reusable LVGL display layers; hardware initialization, pipe behavior, and job state semantics are outside the visual redesign.

## Brand Commitments

- Grobot is the device character and should remain the emotional center of the interface.
- The product personality is a warm creative companion: friendly, precise, and trustworthy.
- The interface must not become a dense engineering console at the primary level, and must not hide necessary machine safety controls.

## Evidence on Hand

- Current LVGL implementation: `main/display/lcd_display.cc`, `main/display/lcd_display.h`, and `main/display/lvgl_display/`.
- Waveshare board wiring and tool callbacks: `main/boards/waveshare/esp32-s3-touch-lcd-3.5/esp32-s3-touch-lcd-3.5.cc`.
- Existing localized strings: `main/assets/locales/zh-CN/language.json` and `main/assets/locales/en-US/language.json`.
- Product and protocol authority: `D:/Users/hutuji/README.md` and `D:/Users/hutuji/docs/protocol.md`.

## Product Principles

1. Voice starts the work; touch makes physical action safe.
2. The primary screen serves family use first; debug power is available but visually subordinate.
3. One glance must answer: what the machine is doing, what action is expected, and whether it is safe to proceed.
4. Destructive or motion-producing actions require explicit, state-aware affordances.
5. Grobot carries personality without competing with previews, status, or controls.

## Accessibility & Inclusion

Child-friendly touch is required: primary touch targets stay at least 56 px high, text must remain readable at arm's length, state must not rely on color alone, and critical actions must use direct Chinese labels rather than icon-only controls.
