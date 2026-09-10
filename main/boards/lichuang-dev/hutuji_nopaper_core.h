#pragma once

// 量产无换纸 SKU 的机型识别与行为判定（纯函数/constexpr，header-only 供 host 测试）。
// 契约：hutuji 枢纽仓 docs/protocol.md §10.4.15（2026-09-10 用户拍板「量产暂时做
// 不换纸机型」「不做多页」）。Grbl 侧分支 massprod/nopaper-1.3a（基线 c896ce5c，
// GRBL_VERSION_BUILD=20260910），树内无 Paper* 代码。
//
// 设计口径：
// - 运行期识别而非编译期——同一 S3 固件服务两种机型；识别锚是 `$I` 应答的
//   `[VER:1.3a.<build>:...]` build 段，新连接（banner/reset）时复位为「未知=换纸机」
//   保守默认，VER 到达前不放宽任何行为。
// - $$ 指纹金标按机型分表：无换纸机没有 §9-G′ 长名锁表（4 月基线无该守卫），
//   末项 `[Errors/Verbose]` 查询必失败，故无换纸表不含该项；数值基线取
//   c896ce5c 的 custom_3axis_hr4988.h 机头默认值（刷机流程须含 $RST=$ 或整片
//   擦写使 NVS 回落机头默认，刷后须 $$ 只读取证校准——见枢纽仓 inventory）。

#include <cstddef>
#include <string>

#include "hutuji_recovery_core.h"

namespace hutuji {

// 无换纸 SKU 的 Grbl build 段真值（与 Grbl.h GRBL_VERSION_BUILD 字面量互为事实源）。
inline constexpr const char* kNopaperGrblBuild = "20260910";

// `$I` 应答行形如 `[VER:1.3a.20260910:]`：取第二个 '.' 后到下一个 ':' 的 build 段。
// 非 VER 行/缺段/其他 build → false（保守：认不出 = 换纸机）。
inline bool GrblVerLineIsNopaperSku(const std::string& line) {
    if (line.rfind("[VER:", 0) != 0) {
        return false;
    }
    const size_t first_dot = line.find('.');
    if (first_dot == std::string::npos) {
        return false;
    }
    const size_t second_dot = line.find('.', first_dot + 1);
    if (second_dot == std::string::npos) {
        return false;
    }
    const size_t end = line.find(':', second_dot + 1);
    const std::string build = line.substr(second_dot + 1, end - second_dot - 1);
    return build == kNopaperGrblBuild;
}

// 页尾换纸编排（M30 + 等换纸完成）是否跳过：无换纸机整个跳过，归位保留。
inline bool NopaperSkipsPaperChange(bool nopaper_machine) { return nopaper_machine; }

// 多页任务（pages>1）在无换纸机上拒绝（不做多页，2026-09-10 用户拍板）。
inline bool NopaperRejectsMultiPage(bool nopaper_machine, size_t page_count) {
    return nopaper_machine && page_count > 1;
}

// 拒绝话术（确认/提交环节一致用这句；短句防 TTS 截断）。
inline constexpr const char* kNopaperMultiPageRejectMsg =
    "这台机器一次只能画一页哦";

// 无换纸机 $$ 指纹金标：数值 = c896ce5c 机头默认值（custom_3axis_hr4988.h：
// $1=255 弹簧笔常使能、$3=bit(Z)=4 只反 Z、$20/21/22=0 无限位、$100/101=100 步进、
// $110/111=5000 机头限速、$130/131=200 行程、$132=20 笔程）。无 §9-G′ 长名项
// （4 月基线无该守卫）。刷机后须实机 $$ 校准本表（NVS 残留会盖过机头默认）。
inline constexpr GrblSettingGolden kGrblSettingGoldensNopaper[] = {
    {"$1", "1", 255.0, true},
    {"$3", "3", 4.0, true},
    {"$20", "20", 0.0, true},
    {"$21", "21", 0.0, true},
    {"$22", "22", 0.0, true},
    {"$100", "100", 100.0, false},
    {"$101", "101", 100.0, false},
    {"$110", "110", 5000.0, false},
    {"$111", "111", 5000.0, false},
    {"$130", "130", 200.0, false},
    {"$131", "131", 200.0, false},
    {"$132", "132", 20.0, false},
};

inline constexpr size_t kGrblSettingGoldenNopaperCount =
    sizeof(kGrblSettingGoldensNopaper) / sizeof(kGrblSettingGoldensNopaper[0]);

// 按机型选当前生效金标表；未知（VER 未达）= 换纸机表（保守，不换纸机行为）。
inline void ActiveGrblSettingGoldens(bool nopaper_machine,
                                     const GrblSettingGolden*& table_out,
                                     size_t& count_out) {
    if (nopaper_machine) {
        table_out = kGrblSettingGoldensNopaper;
        count_out = kGrblSettingGoldenNopaperCount;
    } else {
        table_out = kGrblSettingGoldens;
        count_out = kGrblSettingGoldenCount;
    }
}

}  // namespace hutuji
