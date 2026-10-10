#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <string>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

class SystemInfo {
public:
    static size_t GetFlashSize();
    static size_t GetMinimumFreeHeapSize();
    static size_t GetFreeHeapSize();
    static std::string GetMacAddress();
    static std::string GetChipModelName();
    static std::string GetUserAgent();
    static esp_err_t PrintTaskCpuUsage(TickType_t xTicksToWait);
    static void PrintTaskList();
    static void PrintHeapStats();
    // 取证（2026-09-06）：八路堆指标随取随打，phase 标记调用点（tick/preview/download 出入口）。
    static void LogHeapNow(const char* phase);

    static void PrintPmLocks();
};

#endif // _SYSTEM_INFO_H_
