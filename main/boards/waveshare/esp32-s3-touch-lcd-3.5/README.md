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

显示层启用程序化 Grobot 全脸，Waveshare 使用 `460x300` 画布。热点配网二维码由共享 `Display` / `LcdDisplay` 覆盖层负责，板级代码只组合网络事件。

写字机需要长期在线待命：空闲 60 秒后降低背光，但不再由 AXP2101 在 5 分钟后自动断电；按 BOOT 会先退出省电状态，再执行原有配网或聊天动作。

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

- ESP-IDF v6.0.1 构建通过；host 回归 48/48。
- COM14 实机启动确认 8MB PSRAM、LCD、触摸、音频、OV5640、Grobot `460x300`、Wi-Fi、Grbl Telnet 与授权探测。
- 60 秒省电过渡已观察，无 panic/assert/OOM。
- 尚未完成最终二维码扫码/触摸、BOOT 实体唤醒目视、完整 5 分钟不关机长测、云端 device-call 与真实出图/换纸验证。
