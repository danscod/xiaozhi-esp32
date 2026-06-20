#include "system_info.h"

#include <cstring>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_pm.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#define TAG "SystemInfo"

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free sram: %u minimal sram: %u", free_sram, min_free_sram);
}

void SystemInfo::PrintPmLocks() {
    esp_pm_dump_locks(stdout);
}

size_t SystemInfo::CaptureTaskRunTimes(TaskRunTimeEntry* out_entries, size_t max_entries) {
    if (!out_entries || max_entries == 0) return 0;
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0) return 0;
    TaskStatus_t* arr = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
    if (!arr) return 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n, nullptr);
    size_t written = 0;
    for (UBaseType_t i = 0; i < got && written < max_entries; i++) {
        const char* name = arr[i].pcTaskName ? arr[i].pcTaskName : "?";
        // Skip the idle tasks — they're not "work".
        if (strcmp(name, "IDLE0") == 0 || strcmp(name, "IDLE1") == 0) continue;
        strncpy(out_entries[written].name, name, sizeof(out_entries[written].name) - 1);
        out_entries[written].name[sizeof(out_entries[written].name) - 1] = 0;
        out_entries[written].run_time = arr[i].ulRunTimeCounter;
        out_entries[written].core     = (int)arr[i].xCoreID;
        written++;
    }
    free(arr);
    return written;
}

bool SystemInfo::FindTopTaskByDelta(const TaskRunTimeEntry* start, size_t start_n,
                                    const TaskRunTimeEntry* end,   size_t end_n,
                                    char* out_name, size_t name_buf_len,
                                    uint32_t* out_delta, int* out_core) {
    if (!start || !end || !out_name || !out_delta || !out_core) return false;
    uint32_t best_delta = 0;
    int      best_core  = -1;
    const char* best_name = nullptr;
    for (size_t i = 0; i < end_n; i++) {
        // Find matching name in `start`. Tasks created after start show up
        // only in `end` — for those, treat start runtime as 0.
        uint32_t start_rt = 0;
        for (size_t j = 0; j < start_n; j++) {
            if (strcmp(start[j].name, end[i].name) == 0) {
                start_rt = start[j].run_time;
                break;
            }
        }
        uint32_t end_rt = end[i].run_time;
        // U32 wrap handling.
        uint32_t delta = (end_rt >= start_rt) ? (end_rt - start_rt)
                                              : (uint32_t)(((uint64_t)end_rt + 0x100000000ULL) - start_rt);
        if (delta > best_delta) {
            best_delta = delta;
            best_core  = end[i].core;
            best_name  = end[i].name;
        }
    }
    if (!best_name) return false;
    strncpy(out_name, best_name, name_buf_len - 1);
    out_name[name_buf_len - 1] = 0;
    *out_delta = best_delta;
    *out_core  = best_core;
    return true;
}

bool SystemInfo::GetCoreCpuStats(CoreCpuStats* out) {
    if (!out) return false;
    *out = CoreCpuStats{};
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0) return false;
    TaskStatus_t* arr = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
    if (!arr) return false;
    uint32_t total_runtime = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n, &total_runtime);
    out->total_runtime = total_runtime;
    for (UBaseType_t i = 0; i < got; i++) {
        BaseType_t core = arr[i].xCoreID;
        uint32_t rt = arr[i].ulRunTimeCounter;
        // ESP-IDF's idle tasks are named "IDLE0" / "IDLE1".
        const char* name = arr[i].pcTaskName ? arr[i].pcTaskName : "?";
        bool is_idle0 = (strcmp(name, "IDLE0") == 0);
        bool is_idle1 = (strcmp(name, "IDLE1") == 0);
        if (core == 0)      out->core0_run_time += rt;
        else if (core == 1) out->core1_run_time += rt;
        else                out->no_affinity_run_time += rt;
        if (is_idle0) out->core0_idle_run_time = rt;
        if (is_idle1) out->core1_idle_run_time = rt;
        // Track the biggest non-idle task (skip IDLE0/IDLE1 — they're not work).
        if (!is_idle0 && !is_idle1 && rt > out->top_task_run_time) {
            out->top_task_run_time = rt;
            out->top_task_core = (int)core;
            strncpy(out->top_task_name, name, sizeof(out->top_task_name) - 1);
            out->top_task_name[sizeof(out->top_task_name) - 1] = 0;
        }
    }
    free(arr);
    return true;
}
