#ifndef HUTUJI_BLE_DIAG_CORE_H
#define HUTUJI_BLE_DIAG_CORE_H

#include <array>
#include <cstdint>

namespace hutuji::ble_diag {

inline constexpr uint8_t kPhaseASchemaVersion = 0x10;
inline constexpr uint8_t kPhaseAPayloadBytes = 6;
inline constexpr uint8_t kLinkFlagWifiUp = 0x01;
inline constexpr uint8_t kLinkFlagTelnetReady = 0x02;
inline constexpr uint8_t kKnownLinkFlags = kLinkFlagWifiUp | kLinkFlagTelnetReady;

// 协议 §1.4.3：快照刷新目标 1s，age_s > 5 判 stale。
inline constexpr uint32_t kSnapshotStaleAfterSeconds = 5;

// Legacy advertising 的 AD 数据预算；阶段 A 只放一条 Service Data - 128-bit UUID。
inline constexpr uint8_t kLegacyAdvBudgetBytes = 31;
// length(1) + type(1) + UUID(16) + payload(6)
inline constexpr uint8_t kAdvertisingBytes =
    static_cast<uint8_t>(2 + 16 + kPhaseAPayloadBytes);
inline constexpr uint8_t kAdvTypeServiceData128 = 0x21;

// `d3e6a7b0-7c22-4f61-9b18-2e4d5f6a7001` 的空中线序 = 文档字符串逐字节反序。
// 消费者不得把字符串顺序直接 memcpy 成空中 UUID（协议 §1.4.2）。
inline constexpr std::array<uint8_t, 16> kServiceUuid128 = {
    0x01, 0x70, 0x6a, 0x5f, 0x4d, 0x2e, 0x18, 0x9b,
    0x61, 0x4f, 0x22, 0x7c, 0xb0, 0xa7, 0xe6, 0xd3};

enum class Health : uint8_t {
    Unknown = 0,
    Nominal = 1,
    Degraded = 2,
    Fault = 3,
};

struct PhaseAPayload {
    Health health;
    uint8_t link_flags;
    uint16_t snapshot_seq;
    bool stale;
};

inline constexpr bool IsKnownHealth(Health health) {
    return static_cast<uint8_t>(health) <= static_cast<uint8_t>(Health::Fault);
}

inline constexpr Health NormalizeHealth(Health health) {
    return IsKnownHealth(health) ? health : Health::Unknown;
}

inline constexpr PhaseAPayload MakePhaseAPayload(Health health, uint8_t link_flags,
                                                  uint16_t snapshot_seq, bool stale) {
    return PhaseAPayload{NormalizeHealth(health), static_cast<uint8_t>(link_flags & kKnownLinkFlags),
                         snapshot_seq, stale};
}

inline constexpr std::array<uint8_t, kPhaseAPayloadBytes> SerializePhaseAPayload(
    const PhaseAPayload& payload) {
    return {kPhaseASchemaVersion,
            static_cast<uint8_t>(NormalizeHealth(payload.health)),
            static_cast<uint8_t>(payload.link_flags & kKnownLinkFlags),
            static_cast<uint8_t>(payload.snapshot_seq & 0xffu),
            static_cast<uint8_t>((payload.snapshot_seq >> 8u) & 0xffu),
            static_cast<uint8_t>(payload.stale ? 1u : 0u)};
}

inline constexpr uint16_t NextSnapshotSequence(uint16_t sequence) {
    return static_cast<uint16_t>(sequence + 1u);
}

inline constexpr uint8_t MakeLinkFlags(bool wifi_up, bool telnet_ready) {
    return static_cast<uint8_t>((wifi_up ? kLinkFlagWifiUp : 0u) |
                                (telnet_ready ? kLinkFlagTelnetReady : 0u));
}

// 优先级：stale > fault > 链路缺失 > nominal。stale 时不得猜 nominal/degraded。
inline constexpr Health DeriveHealth(bool wifi_up, bool telnet_ready, bool fault_present,
                                     bool stale) {
    if (stale) {
        return Health::Unknown;
    }
    if (fault_present) {
        return Health::Fault;
    }
    return (wifi_up && telnet_ready) ? Health::Nominal : Health::Degraded;
}

// 取不到单调时钟只能判 stale；age_s 按整秒比较，5000ms 仍新鲜。
inline constexpr bool IsSnapshotStale(bool monotonic_clock_valid, uint32_t age_ms) {
    return !monotonic_clock_valid || age_ms > kSnapshotStaleAfterSeconds * 1000u;
}

// 单条 Service Data - 128-bit UUID AD structure；length 不含自身。
inline constexpr std::array<uint8_t, kAdvertisingBytes> SerializeAdvertisingData(
    const PhaseAPayload& payload) {
    std::array<uint8_t, kAdvertisingBytes> ad{};
    ad[0] = static_cast<uint8_t>(kAdvertisingBytes - 1);
    ad[1] = kAdvTypeServiceData128;
    for (std::size_t i = 0; i < kServiceUuid128.size(); ++i) {
        ad[2 + i] = kServiceUuid128[i];
    }
    const auto body = SerializePhaseAPayload(payload);
    for (std::size_t i = 0; i < body.size(); ++i) {
        ad[2 + kServiceUuid128.size() + i] = body[i];
    }
    return ad;
}

inline constexpr Health HealthForConsumer(Health health, bool stale) {
    return stale ? Health::Unknown : NormalizeHealth(health);
}

}  // namespace hutuji::ble_diag

#endif  // HUTUJI_BLE_DIAG_CORE_H
