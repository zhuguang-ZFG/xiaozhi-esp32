---
version: 1
slug: "main-boards-waveshare-esp32-s3-touch-lcd-3-5"
primary_target: "main/boards/waveshare/esp32-s3-touch-lcd-3.5"
related_targets: ["main/display/lcd_display.cc","main/display/lvgl_display/lvgl_theme.cc"]
---

# Scope and visitor mode

Waveshare ESP32-S3 Touch LCD 3.5 main touch surface; operate mode, with family use first and debug use folded.

# Audience, job, and action

Family users and makers need Grobot to feel warm while physical drawing remains safe. Voice or cloud starts work; the touch surface previews the drawing, asks for confirmation, reports state, and exposes state-aware machine controls.

# Proof and constraints

The surface must preserve hutuji callbacks, machine-control state machines, localized copy keys, 56 px minimum touch targets, Grobot face behavior, and Waveshare LVGL boundaries. Hardware, pipe, and job semantics do not change.

# Chosen direction and memorable moment

Grobot 创作工作台: a dark, quiet creative desk where Grobot is the emotional center, paper-like rounded cards carry preview and controls, cyan brand light marks safe attention, and advanced debug tools stay folded.

# Unresolved decisions

Final visual polish depends on device screenshots and HIL review after the LVGL implementation lands.
