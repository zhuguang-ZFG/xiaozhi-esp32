# 受管组件补丁

`managed_components/` 被 gitignore（`idf.py` 重解析依赖时整目录可重建），本目录存放
对受管组件的本地修复补丁。**每次 `idf.py fullclean` / 依赖版本变更后必须核对本目录
补丁是否仍在位**（编译不报错地静默丢失是本类补丁的最大风险）。

## 在册补丁

| 补丁 | 目标文件 | 内容 | 落地 |
|---|---|---|---|
| `esp-ml307-http-client-disconnect-wakeup.patch` | `managed_components/78__esp-ml307/src/http_client.cc` | `OnTcpDisconnected` 在头部未收齐时补 `xEventGroupSetBits(EC801E_HTTP_EVENT_ERROR)`，唤醒 `GetStatusCode` 的 event-group 等待——否则 SSL 断连后白等满 60s 超时（2026-09-06 预览「卡死一分钟」实机根因之一） | 2026-09-06 随 v6 镜像上板实证（15:19 失败 0.2s 报错，原为 60s 干等） |
| `esp-ml307-ssl-rx-task-and-keepalive.patch` | `managed_components/78__esp-ml307/src/esp/esp_ssl.cc` | SSL 收包任务 `xTaskCreate` 结果校验（内部 RAM 耗尽时快速失败并报错，不再静默死连接）+ TCP 层 keepalive（keepidle 120s / keepintvl 10s / keepcnt 3）。背景：2026-09-06 17:45 页 2 下载 3 连超时的根因是内部 SRAM 碎片化至 L3072，收包任务 4KB 栈分配失败且无诊断 | 2026-09-06 随 TLS 复用镜像上板（HIL 待实证） |
| `esp-ml307-http1-keepalive-default.patch` | `managed_components/78__esp-ml307/src/http_client.cc` + `include/http_client.h` | HTTP/1.1 默认持久连接（RFC 7230 §6.3）：解析 HEADERS 时识别版本置 `response_http_1_1_`，按版本决定收尾行为；`ResetRequestState` 按响应复位。配合 Job 层 TLS 复用，消除每次下载新握手带来的堆 churn | 2026-09-06 随 TLS 复用镜像上板（HIL 待实证） |

## 应用方法


### 循环核验（2026-09-06）

三补丁均已做「反向 `-R` → 干净树正向 `--check` → 正向 apply 恢复」全循环并实测通过（ssl 补丁头
`a/tmp/esp_ssl_pristine.cc` 的 a/b 路径不一致不影响应用，已实证）。注意：**已应用状态下
`git apply --check` 会 FAIL——这是预期**，表示补丁内容已在树上；判「在位」看标记符
（`TCP_KEEPIDLE` / `response_http_1_1_` / OnTcpDisconnected 内 `EC801E_HTTP_EVENT_ERROR` 置位），
判「fullclean 后可再应用」做上反向循环，不要拿 --check 的 FAIL 当补丁丢失。

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
