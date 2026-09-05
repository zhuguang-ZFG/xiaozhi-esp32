# 受管组件补丁

`managed_components/` 被 gitignore（`idf.py` 重解析依赖时整目录可重建），本目录存放
对受管组件的本地修复补丁。**每次 `idf.py fullclean` / 依赖版本变更后必须核对本目录
补丁是否仍在位**（编译不报错地静默丢失是本类补丁的最大风险）。

## 在册补丁

| 补丁 | 目标文件 | 内容 | 落地 |
|---|---|---|---|
| `esp-ml307-http-client-disconnect-wakeup.patch` | `managed_components/78__esp-ml307/src/http_client.cc` | `OnTcpDisconnected` 在头部未收齐时补 `xEventGroupSetBits(EC801E_HTTP_EVENT_ERROR)`，唤醒 `GetStatusCode` 的 event-group 等待——否则 SSL 断连后白等满 60s 超时（2026-09-06 预览「卡死一分钟」实机根因之一） | 2026-09-06 随 v6 镜像上板实证（15:19 失败 0.2s 报错，原为 60s 干等） |

## 应用方法

在本仓根目录：

```powershell
git apply --check patches/<name>.patch   # 先干跑核对上下文
git apply patches/<name>.patch
```

（`git apply` 不要求目标文件被 git 跟踪；路径以仓根为基准。）

升级 `78__esp-ml307` 组件版本前先 `git apply --check`；上下文漂移则按补丁注释手工移植。

## sdkconfig 钉（2026-09-06）

根 `sdkconfig` 被 gitignore，以下关键项已钉进 `sdkconfig.defaults.esp32s3`（tracked）：

- `CONFIG_ESP_WIFI_TX_BUFFER_TYPE=1` + `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y` + `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32`：WiFi TX 缓冲必须动态。静态 16 块会把内部 RAM 钉到 activation 任务 8KB 栈分配失败、启动链静默全停（当日实测：激活挂死、无 OTA/模型/MQTT/NTP）。

**任何 reconfigure / fullclean / 换机构建后，必须复核 `sdkconfig` 里这两项与 defaults 一致**（构建目录 resync 根 sdkconfig 的漂移曾静默覆盖此配置）。验证：`grep -E "TX_BUFFER_TYPE|DYNAMIC_TX_BUFFER_NUM" sdkconfig` 应为 `=1` 和 `=32`。
