# Waveshare ESP32-S3-Touch-LCD-3.5

产品链接：<https://www.waveshare.net/shop/ESP32-S3-Touch-LCD-3.5.htm>

## hutuji 写字机变体

本 fork 将该板作为写字机的 S3 小智板使用，保留原有 ST7796、FT5x06、ES8311、OV5640 与 AXP2101 初始化，并复用 `boards/lichuang-dev/` 的 Telnet 哑管道和任务编排：

- `hutuji.status`
- `hutuji.draw`
- `hutuji.abort`
- `hutuji.pause`
- `hutuji.resume`
- `hutuji.repeat`
- `hutuji.pen_test`

G-code 仍只在云端生成；设备仅下载、校验并通过 Wi-Fi Telnet `:23` 转发到 Grbl。

显示层启用程序化 Grobot 全脸，Waveshare 使用 `460x300` 画布。构图采用黄金比例约束眼距和纵向节奏，并针对未成年人收敛极端情绪：负面状态保留可识别语义，但限制眉压、瞪视、刺激色和持续弹跳；`sleepy` 固定半闭眼、居中视线、嘴角上扬，不眨眼或随机眼跳。视觉原则参考 [M5Stack-Avatar](https://github.com/stack-chan/m5stack-avatar)、[LVGL Kawaii Face](https://github.com/0015/lvgl_kawaii_face) 与 [RoboEyes](https://github.com/FluxGarage/RoboEyes)，未复制第三方实现或引入运行时依赖。

热点配网二维码由共享 `Display` / `LcdDisplay` 覆盖层负责，板级代码只组合网络事件。

写字机需要长期在线待命：空闲 60 秒后降低背光，但不再由 AXP2101 在 5 分钟后自动断电；BOOT 和 FT5x06 首次触摸都会先退出省电状态，再继续原有按键或 LVGL 触摸分发。

## 构建

该板需要显式选择 Kconfig 板型；只传 `BOARD_NAME` 不会自动关闭默认板型。当前开发构建使用独立 build 目录和 defaults 文件，避免污染其他板型的 `sdkconfig`：

```powershell
idf.py -B build-waveshare-35 \
  -DSDKCONFIG=build-waveshare-35/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;tmp/waveshare-35.defaults" \
  -DBOARD_NAME=waveshare/esp32-s3-touch-lcd-3.5 \
  -DBOARD_TYPE=esp32-s3-touch-lcd-3.5 reconfigure
idf.py -B build-waveshare-35 build
```

`tmp/waveshare-35.defaults` 是本机开发输入，不应作为生成物提交；正式发布应把等价板型选择纳入标准 release variant。

## 已验证边界

- ESP-IDF v6.0.1 构建通过；host 回归 54/54。
- COM14 实机启动确认 8MB PSRAM、LCD、触摸、音频、OV5640、Grobot `460x300`、Wi-Fi、Grbl Telnet 与授权探测。
- 儿童友好 UI 已由用户目视接受；触摸退出省电已实机确认。
- 45 秒启动日志无 panic/assert/OOM，最低 SRAM `49551B`。
- 尚未完成最终二维码扫码/触摸、完整 5 分钟不关机长测、云端 device-call 与真实出图/换纸验证。
