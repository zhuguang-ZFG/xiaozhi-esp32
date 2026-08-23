#ifndef PLOTTER_PROVISION_H
#define PLOTTER_PROVISION_H

#include <atomic>
#include <string>

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "plotter_provision_core.h"

namespace hutuji {

/**
 * @brief 设备端零接触配网：S3 借写字机出厂 AP（GRBL_ESP）把户网凭据写进写字机。
 *
 * 触发：户网 Connected 后延迟巡检（先等既有发现，找不到且扫到 GRBL_ESP 才跳配），
 * 或工作台「重新配置写字机」手动入口。流程：扫描出厂热点 → 停 station → 跳连
 * GRBL_ESP → HTTP 逐条写 ESP100/101/110 并校验应答 → ESP444 重启写字机 →
 * 回切户网 → 复用 Pipe 既有发现验证接管。全程 fail-closed：任何失败都先恢复户网。
 *
 * 纯逻辑（命令序列/应答分类/凭据校验/退避）在 plotter_provision_core.h，host 可测。
 *
 * 两个标志分工：task_active_ 保证状态机单实例；busy_ 只覆盖跳网+回切窗口——
 * Pipe::ConnectOnce 据此歇工，不在 192.168.0.x 网段误连写字机 AP 模式的 :23
 * （那会污染 NVS 缓存 IP 并对机器发授权探针）。巡检等待期与验证期 busy_ 都是
 * false，Pipe 照常工作（验证本身就走 Pipe 的既有发现）。
 */
class PlotterProvision {
public:
    static PlotterProvision& GetInstance();

    /** 板级 Connected 事件钩子（网络事件任务上下文）：只布防巡检定时器，不阻塞。 */
    void OnHomeNetworkConnected();

    /** 工作台手动入口（任意上下文）：管道在线时直接提示无需配置。 */
    void RequestManual();

    /** 跳配+回切窗口标志；Pipe::ConnectOnce 据此歇工。验证期已清回 false。 */
    bool IsBusy() const { return busy_.load(); }

private:
    PlotterProvision() = default;
    ~PlotterProvision() = default;
    PlotterProvision(const PlotterProvision&) = delete;
    PlotterProvision& operator=(const PlotterProvision&) = delete;

    static void PatrolTimerCb(void* arg);
    static void ProvisionTaskEntry(void* arg);
    void ProvisionTask(bool manual);
    void ScheduleAttempt(bool manual, uint32_t delay_ms);

    /** 扫描写字机出厂热点是否在场（户网连接态下做，短暂离台 ~2s）。 */
    bool IsFactoryApVisible();

    /** 跳连出厂 AP；成功返回 true 且持有 jump_netif_/jump_events_，失败已自行清理。 */
    bool JumpToFactoryAp();
    /** 跳连收尾：断连、摘事件、停 WiFi、毁 netif——恢复成 StopStation 后的状态。 */
    void TeardownJump();

    /** 逐条发写命令并校验应答，最后发 RESTART。失败写入原因并返回 false。 */
    bool SendCommands(provision::ProvisionFailure* failure);

    /** 等回切户网 Connected；超时返回 false（station 仍在自动重试，不长期离线）。 */
    bool WaitHomeBack();

    /** 结果上报（Schedule 回主循环发通知）并视策略布防下一次自动尝试。 */
    void FinishWith(bool manual, bool ok, provision::ProvisionFailure failure, int attempt);

    static void JumpEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    std::atomic<bool> task_active_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> pending_manual_{false};
    esp_timer_handle_t patrol_timer_ = nullptr;
    esp_timer_handle_t retry_timer_ = nullptr;
    esp_netif_t* jump_netif_ = nullptr;  // 仅 ProvisionTask 读写
    EventGroupHandle_t jump_events_ = nullptr;
    esp_event_handler_instance_t jump_wifi_handler_ = nullptr;
    esp_event_handler_instance_t jump_ip_handler_ = nullptr;
    std::string home_ssid_;      // 仅 ProvisionTask 读写：当前户网凭据（读回自 SsidManager）
    std::string home_password_;  // 脱敏硬约定：永不进日志
    int auto_attempts_ = 0;      // 本轮 Connected 纪元内已消耗的自动尝试
};

}  // namespace hutuji

#endif  // PLOTTER_PROVISION_H
