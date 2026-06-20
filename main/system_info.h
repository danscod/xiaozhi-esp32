#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <string>
#include <cstdint>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

// Snapshot of cumulative per-core runtime counters. Diff two snapshots to
// compute per-core busy percentage over an interval (see GetCoreCpuStats).
struct CoreCpuStats {
    uint64_t total_runtime          = 0; // sampled esp_timer value (us)
    uint64_t core0_run_time         = 0; // sum of ulRunTimeCounter for tasks pinned to core 0
    uint64_t core1_run_time         = 0; // sum for tasks pinned to core 1
    uint64_t no_affinity_run_time   = 0; // sum for tasks with tskNO_AFFINITY
    uint64_t core0_idle_run_time    = 0; // IDLE0 task only — to compute % busy
    uint64_t core1_idle_run_time    = 0; // IDLE1 task only
    char     top_task_name[16]      = {0};
    uint64_t top_task_run_time      = 0;
    int      top_task_core          = -1;
};

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
    static void PrintPmLocks();
    // Capture a per-core CPU runtime snapshot. Two snapshots can be diffed
    // to compute % busy on each core over the interval. Returns false on
    // memory failure (the snapshot allocates a temporary array sized to
    // the current task count).
    static bool GetCoreCpuStats(CoreCpuStats* out);

    // Per-task delta snapshot helpers. The pattern is:
    //   Use CaptureTaskRunTimes(buf) at the start of an interval and at the
    //   end. Then call FindTopTaskByDelta(start_buf, end_buf, ...) to find
    //   the single task that consumed the most CPU during the interval.
    // Buffer size: each task uses sizeof(TaskRunTimeEntry) ~24 bytes.
    struct TaskRunTimeEntry {
        char     name[16];
        uint32_t run_time;
        int      core;
    };
    // Fills out_entries (up to max_entries) with current task snapshot.
    // Returns number of entries written, or 0 on error.
    static size_t CaptureTaskRunTimes(TaskRunTimeEntry* out_entries, size_t max_entries);
    // Returns the index in `end` whose run_time delta is largest, comparing
    // to `start` by name match. Out: name copied to out_name (buf must be
    // >= 16 bytes), delta in ticks to *out_delta, core in *out_core.
    // Returns false if no match found.
    static bool FindTopTaskByDelta(const TaskRunTimeEntry* start, size_t start_n,
                                   const TaskRunTimeEntry* end,   size_t end_n,
                                   char* out_name, size_t name_buf_len,
                                   uint32_t* out_delta, int* out_core);
};

#endif // _SYSTEM_INFO_H_
