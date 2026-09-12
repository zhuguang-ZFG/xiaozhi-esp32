#include "hutuji_ble_diag.h"

#include <sdkconfig.h>

#if defined(CONFIG_HUTUJI_BLE_DIAGNOSTICS)

#include <array>
#include <cinttypes>
#include <cstdint>

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <nimble/nimble_npl.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>

#include <wifi_manager.h>
#include "hutuji_ble_diag_core.h"
#include "hutuji_pipe.h"

#define TAG "hutuji_ble_diag"

namespace hutuji::ble_diag {
namespace {

// 运行时总开关。NVS key 上限 15 字符，故用命名空间 `ble_diag` + key `enabled`
// 承载协议 §1.4.5 的 `ble_diag_enabled` 语义。缺失/非法/读失败一律 false。
constexpr const char* kNvsNamespace = "ble_diag";
constexpr const char* kNvsKeyEnabled = "enabled";

// 快照刷新周期（协议 §1.4.3 目标 1s）。
constexpr uint32_t kSnapshotIntervalMs = 1000;
// NRPA 轮换周期。不广播稳定身份，未绑定客户端不得跨轮换关联同一设备（§1.4.7）。
constexpr uint32_t kAddressRotationMs = 15 * 60 * 1000;
// 需短于 1s 快照刷新周期，否则停播更新可能发生在首个广告事件之前，导致空中不可见。
// 500ms 让每个快照窗口至少容纳一次发送；真实功耗/共存仍以 §1.4.6 HIL 为准。
constexpr uint32_t kAdvIntervalMs = 500;

enum class RestartKind : uint8_t {
    kNone,
    kSnapshot,
    kAddressRotation,
};

struct State {
    ble_npl_callout snapshot_timer;
    ble_npl_callout rotation_timer;
    ble_npl_event advertising_restart_event;
    ble_npl_eventq* eventq = nullptr;
    RestartKind restart_kind = RestartKind::kNone;
    uint16_t snapshot_seq = 0;
    // 上次实际采样 WiFi/Telnet 的单调时刻；publish 时据此算 age 并判 stale，
    // 不能假设定时器一定按时触发（被饿死时必须诚实报 stale）。
    int64_t sampled_at_us = 0;
    bool sampled = false;
    bool advertising = false;
    bool timers_ready = false;
    bool restart_event_ready = false;
    bool disabled = false;
};

State g_state;

/** 运行时闸门：只有显式存成 true 才放行；其余全部按关闭处理。 */
bool RuntimeEnabled() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t enabled = 0;
    const esp_err_t err = nvs_get_u8(handle, kNvsKeyEnabled, &enabled);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    // 只认 1；其它取值视为非法。
    return enabled == 1;
}

/** 采样链路态并组装一份阶段 A payload。单一 owner（NimBLE 主机任务）内调用。 */
PhaseAPayload BuildPayload() {
    const int64_t now_us = esp_timer_get_time();
    const bool wifi_up = WifiManager::GetInstance().IsConnected();
    auto& pipe = Pipe::GetInstance();
    const bool telnet_ready = pipe.IsConnected() && pipe.IsReady();

    // 本次采样即当前时刻；stale 只在时钟不可用或上一份采样已超龄未能刷新时成立。
    const bool clock_valid = now_us > 0;
    uint32_t age_ms = 0;
    if (g_state.sampled && clock_valid && now_us >= g_state.sampled_at_us) {
        age_ms = static_cast<uint32_t>((now_us - g_state.sampled_at_us) / 1000);
    }
    // 首次发布没有前序采样可比，只以时钟有效性判断。
    const bool stale =
        g_state.sampled ? IsSnapshotStale(clock_valid, age_ms) : IsSnapshotStale(clock_valid, 0);

    g_state.sampled = true;
    g_state.sampled_at_us = now_us;
    g_state.snapshot_seq = NextSnapshotSequence(g_state.snapshot_seq);

    // 阶段 A 只暴露 WiFi/Telnet 粗粒度链路态：job、故障分类、机器状态一律不进公开广播。
    // fault 分支留给阶段 B 的认证 GATT 面。
    const Health health = DeriveHealth(wifi_up, telnet_ready, /*fault_present=*/false, stale);
    return MakePhaseAPayload(health, MakeLinkFlags(wifi_up, telnet_ready), g_state.snapshot_seq,
                             stale);
}

void Shutdown(const char* reason);

bool ArmSnapshotTimer() {
    const int rc = ble_npl_callout_reset(&g_state.snapshot_timer,
                                         ble_npl_time_ms_to_ticks32(kSnapshotIntervalMs));
    if (rc != 0) {
        ESP_LOGE(TAG, "snapshot timer arm failed rc=%d", rc);
        Shutdown("snapshot_timer_arm_failed");
        return false;
    }
    return true;
}

bool ArmRotationTimer() {
    const int rc = ble_npl_callout_reset(&g_state.rotation_timer,
                                         ble_npl_time_ms_to_ticks32(kAddressRotationMs));
    if (rc != 0) {
        ESP_LOGE(TAG, "rotation timer arm failed rc=%d", rc);
        Shutdown("rotation_timer_arm_failed");
        return false;
    }
    return true;
}

/** 把当前快照写进广播数据。返回 NimBLE rc。 */
int PublishSnapshot() {
    static_assert(kAdvertisingBytes <= kLegacyAdvBudgetBytes,
                  "阶段 A 只允许一条 Service Data AD structure");
    const auto ad = SerializeAdvertisingData(BuildPayload());
    return ble_gap_adv_set_data(ad.data(), static_cast<int>(ad.size()));
}

/** 生成并设置一枚新 NRPA。失败即 fail-closed，绝不回退公开 MAC。 */
int ApplyFreshNrpa() {
    ble_addr_t addr{};
    // nrpa=1：不可解析私有地址，不需要 IRK/隐私栈，也无法被未绑定客户端长期关联。
    const int rc = ble_hs_id_gen_rnd(1, &addr);
    if (rc != 0) {
        return rc;
    }
    return ble_hs_id_set_rnd(addr.val);
}

int StartAdvertising() {
    ble_gap_adv_params params{};
    // non-connectable + non-scannable：不开连接/配对路径，也不回 scan response。
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_NON;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(kAdvIntervalMs);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(kAdvIntervalMs);

    // 阶段 A 无连接事件可处理，故不注册 GAP 事件回调。
    return ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER, &params, nullptr,
                             nullptr);
}

void OnAdvertisingRestart(ble_npl_event* event);

int QueueAdvertisingRestart(RestartKind kind) {
    // 定时器事件可能同时到达；同一 event 只允许排队一次，地址轮换优先于普通刷新。
    if (g_state.eventq != nullptr && g_state.restart_event_ready &&
        ble_npl_event_is_queued(&g_state.advertising_restart_event)) {
        if (kind == RestartKind::kAddressRotation) {
            g_state.restart_kind = kind;
        }
        return 0;
    }
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        return rc;
    }
    g_state.advertising = false;
    g_state.restart_kind = kind;
    if (g_state.eventq == nullptr || !g_state.restart_event_ready) {
        return BLE_HS_EUNKNOWN;
    }
    ble_npl_eventq_put(g_state.eventq, &g_state.advertising_restart_event);
    return 0;
}

void OnAdvertisingRestart(ble_npl_event* /*event*/) {
    if (g_state.disabled) {
        return;
    }
    const RestartKind kind = g_state.restart_kind;
    g_state.restart_kind = RestartKind::kNone;
    if (kind == RestartKind::kNone) {
        return;
    }
    if (kind == RestartKind::kAddressRotation) {
        const int rc = ApplyFreshNrpa();
        if (rc != 0) {
            ESP_LOGE(TAG, "nrpa rotation failed rc=%d", rc);
            Shutdown("address_rotation_failed");
            return;
        }
    }
    int rc = PublishSnapshot();
    if (rc != 0) {
        ESP_LOGE(TAG, "adv data after stop failed rc=%d", rc);
        Shutdown("advertising_data_refresh_failed");
        return;
    }
    rc = StartAdvertising();
    if (rc != 0) {
        ESP_LOGE(TAG, "adv restart failed rc=%d", rc);
        Shutdown("advertising_restart_failed");
        return;
    }
    g_state.advertising = true;
    if (!ArmSnapshotTimer()) {
        return;
    }
    if (kind == RestartKind::kAddressRotation) {
        ESP_LOGI(TAG, "ble_surface=advertising_only address rotated");
        if (!ArmRotationTimer()) {
            return;
        }
    }
}

int RefreshAdvertising() {
    // NimBLE 明确要求：在 host task 内 stop 后，后续广播操作必须排到独立 event。
    // set_data 也放到该边界之后，避免 controller 仍处理 stop 时收到下一条 HCI 命令。
    return QueueAdvertisingRestart(RestartKind::kSnapshot);
}

void OnSnapshotTimer(ble_npl_event* /*event*/) {
    if (!g_state.advertising || g_state.disabled) {
        return;
    }
    const int rc = RefreshAdvertising();
    if (rc != 0) {
        ESP_LOGE(TAG, "adv data refresh failed rc=%d", rc);
        Shutdown("snapshot_publish_failed");
    }
}

void OnRotationTimer(ble_npl_event* /*event*/) {
    if (g_state.disabled) {
        return;
    }
    // 若快照刷新已停播并排队，本次请求会把同一 pending event 升级为地址轮换；
    // 不能重排 15 分钟后再做，否则恰逢刷新时会漏掉整次隐私轮换。
    const int rc = QueueAdvertisingRestart(RestartKind::kAddressRotation);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv stop for rotation failed rc=%d", rc);
        Shutdown("address_rotation_stop_failed");
    }
}

/**
 * fail-closed 关停诊断广播：停定时器、停广播。只影响诊断面——不碰
 * WiFi/Telnet/job，不重启整机（协议 §1.4.5）。
 * 阶段 A 不提供自愈重启：关掉就保持关闭，直到重新刷写/重启设备。
 * 从 NimBLE 主机任务内调用，故不在此拆 host/controller（nimble_port_stop()
 * 必须在主机任务之外执行，否则自锁）。
 */
void Shutdown(const char* reason) {
    if (g_state.disabled) {
        return;
    }
    g_state.disabled = true;
    if (g_state.timers_ready) {
        ble_npl_callout_stop(&g_state.snapshot_timer);
        ble_npl_callout_stop(&g_state.rotation_timer);
    }
    if (g_state.restart_event_ready && g_state.eventq != nullptr &&
        ble_npl_event_is_queued(&g_state.advertising_restart_event)) {
        ble_npl_eventq_remove(g_state.eventq, &g_state.advertising_restart_event);
    }
    g_state.restart_kind = RestartKind::kNone;
    if (g_state.advertising) {
        const int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "adv stop during shutdown failed rc=%d", rc);
        }
        g_state.advertising = false;
    }
    ESP_LOGE(TAG, "ble diagnostics disabled reason=%s", reason);
}

void OnHostReset(int reason) {
    // controller/host 复位是异常路径：不重建广播，按 fail-closed 收摊。
    ESP_LOGE(TAG, "nimble host reset reason=%d", reason);
    g_state.advertising = false;
    Shutdown("host_reset");
}

void OnHostSync() {
    if (g_state.disabled) {
        ESP_LOGE(TAG, "host sync ignored after fail-closed shutdown");
        return;
    }
    int rc = ApplyFreshNrpa();
    if (rc != 0) {
        ESP_LOGE(TAG, "nrpa generation failed rc=%d", rc);
        Shutdown("nrpa_generation_failed");
        return;
    }

    g_state.eventq = nimble_port_get_dflt_eventq();
    if (g_state.eventq == nullptr) {
        ESP_LOGE(TAG, "nimble event queue unavailable");
        Shutdown("event_queue_unavailable");
        return;
    }
    ble_npl_event_init(&g_state.advertising_restart_event, OnAdvertisingRestart, nullptr);
    g_state.restart_event_ready = true;
    rc = ble_npl_callout_init(&g_state.snapshot_timer, g_state.eventq, OnSnapshotTimer, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "snapshot callout init failed rc=%d", rc);
        Shutdown("snapshot_callout_init_failed");
        return;
    }
    rc = ble_npl_callout_init(&g_state.rotation_timer, g_state.eventq, OnRotationTimer, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "rotation callout init failed rc=%d", rc);
        ble_npl_callout_deinit(&g_state.snapshot_timer);
        Shutdown("rotation_callout_init_failed");
        return;
    }
    g_state.timers_ready = true;

    rc = PublishSnapshot();
    if (rc != 0) {
        ESP_LOGE(TAG, "initial adv data failed rc=%d", rc);
        Shutdown("advertising_data_failed");
        return;
    }
    rc = StartAdvertising();
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
        Shutdown("advertising_start_failed");
        return;
    }
    g_state.advertising = true;

    if (!ArmSnapshotTimer() || !ArmRotationTimer()) {
        return;
    }

    // 启动身份必须明确报告实际暴露面，禁止只说“BLE 已启用”（协议 §1.4.1）。
    ESP_LOGW(TAG,
             "ble_surface=advertising_only schema=BLE-DIAG-v1.0 payload=%u adv_bytes=%u "
             "connectable=no scannable=no gatt=no addr=nrpa rotate_s=%" PRIu32,
             static_cast<unsigned>(kPhaseAPayloadBytes), static_cast<unsigned>(kAdvertisingBytes),
             kAddressRotationMs / 1000);
}

void HostTask(void* /*param*/) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

void Start() {
    if (!RuntimeEnabled()) {
        // 默认路径：不初始化 controller，不广播，不占内存。
        ESP_LOGI(TAG, "ble_surface=disabled (runtime switch off)");
        return;
    }

    // nimble_port_init() 一步做 controller init + enable + host init，且自带
    // 失败回滚（enable 失败 deinit、host init 失败 disable+deinit）。手写这段
    // 序列只会与 IDF 的回滚语义漂移。
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble port init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = OnHostReset;
    ble_hs_cfg.sync_cb = OnHostSync;
    // 阶段 A 不注册 GATT service、不开 bonding/SM：没有可连接面就没有配对面。

    err = esp_nimble_enable(reinterpret_cast<void*>(HostTask));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble enable failed: %s", esp_err_to_name(err));
        nimble_port_deinit();
        return;
    }
    // 广播在 OnHostSync 内启动：controller 同步前不能设地址或广播数据。
}

}  // namespace hutuji::ble_diag

#else  // !CONFIG_HUTUJI_BLE_DIAGNOSTICS

namespace hutuji::ble_diag {

// 默认构建：整个 BLE 诊断面不进镜像，调用点是空实现。
void Start() {}

}  // namespace hutuji::ble_diag

#endif  // CONFIG_HUTUJI_BLE_DIAGNOSTICS
