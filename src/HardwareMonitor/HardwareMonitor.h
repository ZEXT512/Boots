#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "../common/Result.h"
#include "../common/Logger.h"

/**
 * Simple wrapper for a PDH query that automatically adds and removes counters.
 */
class PdhQueryWrapper {
public:
    PdhQueryWrapper();
    ~PdhQueryWrapper();
    Result<void> add_counter(const std::wstring& counterPath);
    Result<double> get_counter_value(size_t index) const; // returns raw double value
    size_t counter_count() const { return _counters.size(); }
    Result<void> collect();
private:
    HQUERY _query = nullptr;
    std::vector<HCOUNTER> _counters;
};

/**
 * Representation of a single measurement snapshot.
 */
struct HardwareSnapshot {
    // CPU
    double cpu_total_percent = 0.0;                // overall usage
    std::vector<double> cpu_per_core_percent;      // per‑core usage
    std::vector<double> cpu_core_clock_mhz;        // current clock per core (if available)
    std::vector<double> cpu_core_temp_c;           // temperature per core (optional, may be empty)

    // RAM
    unsigned long long mem_total_bytes = 0;
    unsigned long long mem_used_bytes = 0;
    unsigned long long mem_available_bytes = 0;
    double mem_pressure_percent = 0.0; // derived metric (used / total * 100)

    // GPU – we attempt to query via DXGI and WMI. Missing fields are left at -1.
    double gpu_util_percent = -1.0;
    unsigned long long gpu_vram_total_bytes = 0;
    unsigned long long gpu_vram_used_bytes = 0;
    unsigned long long gpu_shared_memory_bytes = 0;
    double gpu_engine_util_percent = -1.0; // could be compute/graphics combined
    double gpu_temp_c = -1.0;

    // Disk
    double disk_read_bytes_per_sec = 0.0;
    double disk_write_bytes_per_sec = 0.0;
    double disk_response_time_ms = -1.0;
    double disk_active_time_percent = -1.0;

    // Timestamp of capture
    std::chrono::system_clock::time_point timestamp;
};

/**
 * HardwareMonitor – gathers real‑time metrics using Windows performance counters and
 * DXGI/WMI where possible. All operations are non‑blocking; the user should call
 * start() to launch a background thread that updates the latest snapshot at the
 * desired interval (e.g., 500 ms). The UI can retrieve a copy via get_latest().
 *
 * Safety: If a sensor is unavailable, the corresponding field is set to -1 (or
 * an empty vector) and the UI must label it as "N/A".
 */
class HardwareMonitor {
public:
    ~HardwareMonitor() { stop(); }
    Result<void> start(unsigned int poll_interval_ms = 500); // launch background thread
    Result<void> stop();
    HardwareSnapshot get_latest() const; // snapshot copy (thread‑safe)
    // Simple analysis – returns a list of textual warnings.
    std::vector<std::string> analyze_performance(const HardwareSnapshot& snap) const;
private:
    void monitoring_loop(unsigned int interval_ms);
    mutable std::mutex _mtx;
    std::thread _worker;
    std::atomic<bool> _running{false};
    unsigned int _interval_ms = 500;
    HardwareSnapshot _latest;

    // Internal helpers for each subsystem
    Result<void> init_cpu_counters();
    Result<void> init_ram_counters();
    Result<void> init_disk_counters();
    Result<void> query_gpu(HardwareSnapshot& snap); // fills GPU fields (may fallback to N/A)

    bool _initialized = false;

    // PDH query wrappers for each subsystem
    PdhQueryWrapper _cpu_query;
    PdhQueryWrapper _cpu_core_query;
    PdhQueryWrapper _ram_query;
    PdhQueryWrapper _disk_query;
};
