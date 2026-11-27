// BOOTFPS — HardwareMonitor implementation.
//
// Real PDH-backed sampler: CPU total + per-core, RAM, disk throughput and
// GPU engine utilisation (via the "GPU Engine" counter set when available).
// The PdhQueryWrapper implementation also lives here so every module that
// links HardwareMonitor gets it (Benchmark links HardwareMonitor).
#include "HardwareMonitor.h"

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "pdh.lib")

// ── PdhQueryWrapper (RAII) ───────────────────────────────────────────────
PdhQueryWrapper::PdhQueryWrapper() {
    if (PdhOpenQueryW(nullptr, 0, &_query) != ERROR_SUCCESS) {
        _query = nullptr;
    }
}

PdhQueryWrapper::~PdhQueryWrapper() {
    if (_query) PdhCloseQuery(_query);
}

Result<void> PdhQueryWrapper::add_counter(const std::wstring& counterPath) {
    if (!_query) return Result<void>::Err("PDH query not opened.");

    HCOUNTER hCounter = nullptr;
    // Prefer the locale-independent English counter names. On systems where
    // the English set is unavailable fall back to the localized path.
    DWORD rc = PdhAddEnglishCounterW(_query, counterPath.c_str(), 0, &hCounter);
    if (rc != ERROR_SUCCESS) {
        rc = PdhAddCounterW(_query, counterPath.c_str(), 0, &hCounter);
    }
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("Failed to add PDH counter: " + std::to_string(rc));
    }
    _counters.push_back(hCounter);
    return Result<void>::Ok();
}

Result<void> PdhQueryWrapper::collect() {
    if (!_query) return Result<void>::Err("PDH query not opened.");
    DWORD rc = PdhCollectQueryData(_query);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("PdhCollectQueryData failed: " + std::to_string(rc));
    }
    return Result<void>::Ok();
}

Result<double> PdhQueryWrapper::get_counter_value(size_t index) const {
    if (index >= _counters.size()) return Result<double>::Err("Counter index out of range");
    PDH_FMT_COUNTERVALUE counterVal{};
    DWORD rc = PdhGetFormattedCounterValue(_counters[index], PDH_FMT_DOUBLE, nullptr, &counterVal);
    if (rc != ERROR_SUCCESS) {
        return Result<double>::Err("PdhGetFormattedCounterValue failed: " + std::to_string(rc));
    }
    return Result<double>::Ok(counterVal.doubleValue);
}

// ── GPU engine query ─────────────────────────────────────────────────────
// "GPU Engine(*)" expands to many instances, so it needs the counter-array
// API rather than the single-value helper above.
namespace {
    struct GpuEngineQuery {
        HQUERY query = nullptr;
        HCOUNTER counter = nullptr;
        bool tried = false;
        std::mutex mutex;

        ~GpuEngineQuery() {
            if (query) PdhCloseQuery(query);
        }
    };

    GpuEngineQuery& gpu_engine_query() {
        static GpuEngineQuery instance;
        return instance;
    }
}

// ── HardwareMonitor ──────────────────────────────────────────────────────
Result<void> HardwareMonitor::init_cpu_counters() {
    Result<void> total = _cpu_query.add_counter(L"\\Processor(_Total)\\% Processor Time");
    if (total.is_err()) {
        Logger::instance().log(Logger::Level::Warn, L"CPU total counter unavailable.");
    }

    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    const unsigned cores = std::max<unsigned>(1, sysInfo.dwNumberOfProcessors);
    for (unsigned i = 0; i < cores; ++i) {
        std::wstring path = L"\\Processor(" + std::to_wstring(i) + L")\\% Processor Time";
        _cpu_core_query.add_counter(path); // per-core counters are best effort
    }
    return Result<void>::Ok();
}

Result<void> HardwareMonitor::init_ram_counters() {
    // RAM totals come from GlobalMemoryStatusEx; PDH provides the live
    // available figure as a cross-check on systems with dynamic page files.
    _ram_query.add_counter(L"\\Memory\\Available MBytes");
    return Result<void>::Ok();
}

Result<void> HardwareMonitor::init_disk_counters() {
    _disk_query.add_counter(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec");
    _disk_query.add_counter(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec");
    return Result<void>::Ok();
}

Result<void> HardwareMonitor::query_gpu(HardwareSnapshot& snap) {
    GpuEngineQuery& gpu = gpu_engine_query();
    std::lock_guard<std::mutex> lock(gpu.mutex);

    if (!gpu.tried) {
        gpu.tried = true;
        if (PdhOpenQueryW(nullptr, 0, &gpu.query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(gpu.query,
                                      L"\\GPU Engine(*)\\Utilization Percentage",
                                      0, &gpu.counter) != ERROR_SUCCESS) {
                gpu.counter = nullptr;
            }
        }
    }
    if (!gpu.counter) return Result<void>::Err("GPU Engine counters unavailable.");

    if (PdhCollectQueryData(gpu.query) != ERROR_SUCCESS) {
        return Result<void>::Err("GPU query collection failed.");
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    DWORD rc = PdhGetFormattedCounterArrayW(gpu.counter, PDH_FMT_DOUBLE,
                                            &bufferSize, &itemCount, nullptr);
    if (rc != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0) {
        return Result<void>::Err("GPU counter array unavailable.");
    }

    std::vector<unsigned char> buffer(bufferSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    rc = PdhGetFormattedCounterArrayW(gpu.counter, PDH_FMT_DOUBLE,
                                      &bufferSize, &itemCount, items);
    if (rc != ERROR_SUCCESS) return Result<void>::Err("GPU counter array read failed.");

    double busiest = 0.0;
    for (DWORD i = 0; i < itemCount; ++i) {
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
            busiest = std::max(busiest, items[i].FmtValue.doubleValue);
        }
    }
    busiest = std::min(100.0, std::max(0.0, busiest));
    snap.gpu_util_percent = busiest;
    snap.gpu_engine_util_percent = busiest;
    return Result<void>::Ok();
}

void HardwareMonitor::monitoring_loop(unsigned int interval_ms) {
    // Prime the counters so the first delta-based reading is meaningful.
    _cpu_query.collect();
    _cpu_core_query.collect();
    _ram_query.collect();
    _disk_query.collect();

    while (_running) {
        HardwareSnapshot snap;
        snap.timestamp = std::chrono::system_clock::now();

        _cpu_query.collect();
        auto cpu = _cpu_query.get_counter_value(0);
        if (cpu.is_ok()) snap.cpu_total_percent = std::min(100.0, std::max(0.0, cpu.unwrap()));

        _cpu_core_query.collect();
        snap.cpu_per_core_percent.reserve(_cpu_core_query.counter_count());
        for (size_t i = 0; i < _cpu_core_query.counter_count(); ++i) {
            auto core = _cpu_core_query.get_counter_value(i);
            if (core.is_ok()) {
                snap.cpu_per_core_percent.push_back(std::min(100.0, std::max(0.0, core.unwrap())));
            }
        }

        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            snap.mem_total_bytes = memory.ullTotalPhys;
            snap.mem_available_bytes = memory.ullAvailPhys;
            snap.mem_used_bytes = memory.ullTotalPhys - memory.ullAvailPhys;
            if (snap.mem_total_bytes > 0) {
                snap.mem_pressure_percent =
                    (static_cast<double>(snap.mem_used_bytes) * 100.0) /
                    static_cast<double>(snap.mem_total_bytes);
            }
        }

        _disk_query.collect();
        auto read = _disk_query.get_counter_value(0);
        if (read.is_ok()) snap.disk_read_bytes_per_sec = std::max(0.0, read.unwrap());
        auto write = _disk_query.get_counter_value(1);
        if (write.is_ok()) snap.disk_write_bytes_per_sec = std::max(0.0, write.unwrap());

        query_gpu(snap); // leaves the GPU fields at -1 when unavailable

        {
            std::lock_guard<std::mutex> lock(_mtx);
            _latest = std::move(snap);
        }

        // Sleep in short slices so stop() is responsive.
        unsigned int slept = 0;
        while (_running && slept < interval_ms) {
            unsigned int slice = std::min<unsigned int>(100, interval_ms - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            slept += slice;
        }
    }
}

Result<void> HardwareMonitor::start(unsigned int poll_interval_ms) {
    if (_running) return Result<void>::Ok();

    if (!_initialized) {
        init_cpu_counters();
        init_ram_counters();
        init_disk_counters();
        _initialized = true;
    }

    _interval_ms = std::max<unsigned int>(100, poll_interval_ms);
    _running = true;
    _worker = std::thread(&HardwareMonitor::monitoring_loop, this, _interval_ms);
    Logger::instance().log(Logger::Level::Info, L"HardwareMonitor started.");
    return Result<void>::Ok();
}

Result<void> HardwareMonitor::stop() {
    _running = false;
    if (_worker.joinable()) _worker.join();
    Logger::instance().log(Logger::Level::Info, L"HardwareMonitor stopped.");
    return Result<void>::Ok();
}

HardwareSnapshot HardwareMonitor::get_latest() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _latest;
}

std::vector<std::string> HardwareMonitor::analyze_performance(const HardwareSnapshot& snap) const {
    std::vector<std::string> warnings;

    if (snap.cpu_total_percent >= 90.0) {
        warnings.push_back("CPU usage is saturated (" +
                           std::to_string(static_cast<int>(snap.cpu_total_percent)) +
                           "%). Background work may be stealing frames.");
    }
    if (snap.mem_pressure_percent >= 90.0) {
        warnings.push_back("Memory pressure is high (" +
                           std::to_string(static_cast<int>(snap.mem_pressure_percent)) +
                           "%). The page file may be active.");
    }
    if (snap.gpu_util_percent >= 95.0) {
        warnings.push_back("GPU is fully loaded – the render queue is the bottleneck.");
    }
    if (snap.disk_active_time_percent >= 90.0) {
        warnings.push_back("Disk is almost constantly busy; asset streaming may stutter.");
    }
    if (snap.gpu_temp_c > 85.0) {
        warnings.push_back("GPU temperature above 85 C – thermal throttling is likely.");
    }
    for (size_t i = 0; i < snap.cpu_core_temp_c.size(); ++i) {
        if (snap.cpu_core_temp_c[i] > 90.0) {
            warnings.push_back("CPU core " + std::to_string(i) + " above 90 C.");
            break;
        }
    }
    return warnings;
}
