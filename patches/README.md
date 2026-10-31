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
| `esp-ml307-tls-socket-lifecycle.patch` | `managed_components/78__esp-ml307/src/esp/esp_ssl.cc` + `src/esp/esp_ssl.h` | Disconnect 先 shutdown 保留 sockfd，接收线程退出后 `esp_tls_conn_destroy` 唯一 close。最多等待 3×10s；超时放弃 TLS 所有权并泄漏事件组，避免无限阻塞或对仍在跑的接收线程 UAF。放弃后 `Connect` 直接失败。读失败记录 ret/errno/内部堆，主动关闭不冒充网络失败 | 2026-10-22 1.0.47 起登记；1.0.48 收紧有限等待；1.0.49 拒绝重用已放弃对象 |
| `esp-ml307-z-tls-abandon-leak.patch` | `include/tcp.h` + `include/http_client.h` + `src/http_client.cc` | `Tcp::ReceiveAbandoned` 默认 false；HttpClient 在 Close/Open 替换/析构见放弃标志则 `tcp_.release()`，避免 unique_ptr 析构仍在跑的 EspSsl。字母序排在既有四份之后，CI glob 最后应用 | 2026-10-22 1.0.49 登记。不把源码修复写成已上板 UAF 事故 |

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

2026-10-22 TLS生命周期补丁：esp-ml307-tls-socket-lifecycle.patch在三份既有补丁之后应用，Disconnect先shutdown保留socket编号，接收线程退出后esp_tls_conn_destroy唯一close。消除旧TLS二次close关闭被其他HTTP/MQTT/板间连接复用的fd。最多3×10s等待；超时放弃TLS所有权并泄漏事件组，不无限阻塞、不destroy仍被接收线程使用的对象。1.0.49 起放弃对象禁止 Connect，并由字母序最后的 esp-ml307-z-tls-abandon-leak.patch 在 HttpClient 路径泄漏整个 EspSsl。非主动关闭的读失败记录ret/errno和内部堆，主动关闭不冒充网络失败。真实函数描述符交错回归test_tls_lifecycle.py覆盖关闭/读失败/幂等/放弃退出，源码断言覆盖 leak/拒绝重用，不把源码修复等同历史OTA唯一根因或已验证出货。

2026-10-22 F49：`esp-ml307-zz-shared-receive-state.patch` 必须在上述补丁之后应用。HTTP/WebSocket 公共对象持有共享接收 State，TCP 回调弱引用 State、执行期间提升强引用；关闭标记退休、有限等待并唤醒 HTTP 背压，旧 State 不被新请求复用。放弃接收者后保留借用 TLS 指针，拒绝继续发送/重连；State 析构仅释放已放弃 Tcp 的所有权，避免接收线程访问已释放对象。正常 HTTP 同源 keepalive 保留，跨源重建；握手失败和未连接析构也回收。完整 HTTP/WebSocket 宿主回归见 `scripts/tests/test_transport_owner_lifetime.py`；版本 1.0.50，构建与上板状态分开记录于枢纽 readiness。
