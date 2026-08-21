# Waveshare ESP32-S3-Touch-LCD-3.5

产品链接：<https://www.waveshare.net/shop/ESP32-S3-Touch-LCD-3.5.htm>

## hutuji 写字机变体

本 fork 将该板作为写字机的 S3 小派板使用，保留原有 ST7796、FT5x06、ES8311、OV5640 与 AXP2101 初始化，并复用 `boards/lichuang-dev/` 的 Telnet 哑管道和任务编排：

- `hutuji.status`
- `hutuji.draw`
- `hutuji.abort`
- `hutuji.pause`
- `hutuji.resume`
- `hutuji.repeat`
- `hutuji.pen_test`
- `hutuji.confirm`（屏幕预览确认出图）
- `hutuji.sing` / `hutuji.stop_song`（唱歌，与绘图解耦，只走 AudioService 播放泵；契约见枢纽 `docs/protocol.md` §10）

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

- ESP-IDF v6.0.1 构建通过；host 回归 93/93，相关 C++ 通过仓库 `.clang-format` 的 dry-run 检查。
- COM14 已刷入当前应用镜像：`xiaozhi.bin` 2,974,896B、SHA-256 `966bdf3d…a1adf`，esptool 写入后哈希校验通过；随后约 60 秒持续收到 Grbl `Idle`，最低 SRAM 27,803B，无 panic/assert/watchdog。
- FT6X36 实机寄存器为 threshold `70`、active period `12`、chip `0x64`、vendor `0x11`；启动时把 threshold 调到 `40` 并回读确认。官方 demo 的 display `(swap=1, mirror_x=1, mirror_y=1)` 配 touch `(1,0,1)`；本仓 display 为 `(1,0,0)`，故触摸改为与其配对的 `(1,1,0)`。旧绝对照搬 `(1,0,1)` 的版本实机按钮未命中，当前矩阵已由按钮可见和可拖动的实机反馈覆盖。
- 主屏控制入口使用 `TOP_LEFT` 父坐标、固定创建尺寸与绝对触点锚定拖动：对象关闭滚动/滚动链并启用 `PRESS_LOCK`，6px 仅区分点按与拖动，不吞掉阈值前位移。轻点在 `RELEASED` 打开抽屉；抽屉提供暂停、继续、停止、重画、试笔，「点动·手动」区（原「高级调试」，2026-08-20 更名并改为展开态跨抽屉开合保持，开机默认折叠不变）折叠抬落笔、点动、原点、解锁、关电机和复位。抽屉面板可垂直滚动，面板内按钮一律 `CLICKED`——从按钮起手拖成滚动时 LVGL 只发 `PRESS_LOST`、释放不发 `CLICKED`，reset/set_origin 等高危键天然免疫滚屏误触；点动越界判定 fail-closed：先发 `?` 取「新状态报告 + 新有限 MPos」双序号（`QueryAndWaitFreshMachineState`，与丢 ok 兜底共用），再交 `DecideJog` 判 X≤190/Y≤277 包线（与云端 protocol §5 同源），取不到新鲜坐标或越界即拒绝——无限位开关机器上这是点动的唯一防线。二维码与出图预览显示时隐藏入口，所有机器动作仍只调度既有 Job API。
- Grobot 使用独立 40px 单行字幕层，不依赖 WeChat `bottom_bar_`；用户已目视确认字幕恢复。`screen`、纸感舞台、表情容器与 canvas 均关闭滚动及滚动链，禁止触摸拖走脸层。省电改为 180 秒后降至 35%，自动关机保持禁用。
- 换纸机械、时序、运动、传感器与错误码全部由 Grbl `user_m30()/paper_auto_change()` 实现；S3 页尾只发唯一 `M30` 并等待最终 `ok/error`，测试禁止任何纸路电机命令进入 S3 页尾事务。独立 `G1 X0Y0` 只实现画完回左下角，Grbl 的 `M30` 不归位。
- 当前绝对锚定拖动镜像已构建、刷写并通过窄启动检查；用户此前已确认入口可见且增量版可拖动，当前版慢拖/快拖跟手性仍待现场复核。预览确认/取消、抽屉各机器动作和绘图中语音并发仍待最终 HIL，不宣称实机通过。
