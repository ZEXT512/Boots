#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <windows.h>
#include <pdh.h>

#include "../common/Result.h"
#include "../common/Logger.h"
#include "../HardwareMonitor/HardwareMonitor.h"   // PdhQueryWrapper

/**
 * Simple struct representing a single frame measurement.
 */
struct FrameData {
    double fps;           // Frames per second for this frame (1 / delta)
    double frame_time_ms; // Frame time in milliseconds
};

/**
 * BenchmarkResult – aggregated metrics for a benchmark run.
 */
struct BenchmarkResult {
    double avg_fps = 0.0;
    double low_1_percent_fps = 0.0; // 1% low FPS
    double avg_frame_time_ms = 0.0;
    double cpu_usage_percent = 0.0;
    double gpu_usage_percent = 0.0;
    double ram_usage_mb = 0.0;
    double disk_activity_mb_s = 0.0;
    std::vector<FrameData> frames; // raw data (optional for detailed report)
    std::chrono::system_clock::time_point timestamp;
};

/**
 * PerformanceBenchmark – runs a lightweight stress test (e.g., a continuously
 * rotating 3‑D cube rendered with DirectX) and measures FPS, CPU/GPU load,
 * memory and disk activity. The implementation uses PDH counters for system
 * usage and a high‑resolution timer for frame timing. The UI can call the
 * module twice (before and after optimisation) and then compare the results.
 */
class PerformanceBenchmark {
public:
    Result<void> initialize();
    Result<void> shutdown();

    // Run the benchmark and return aggregated results.
    // The caller may specify a duration in seconds (default 10).
    Result<BenchmarkResult> run(double duration_seconds = 10.0);

    // Generate JSON report (string) for a pair of results (baseline, after).
    std::string generate_json_report(const BenchmarkResult& before,
                                     const BenchmarkResult& after) const;

    // Generate a simple HTML report (string). The UI can write it to a file.
    std::string generate_html_report(const BenchmarkResult& before,
                                     const BenchmarkResult& after) const;

    // Compare two results and return textual findings.
    std::vector<std::string> compare(const BenchmarkResult& before,
                                     const BenchmarkResult& after) const;

    // Verify that a list of expected settings were applied.
    // The "applied" flag should be true for each setting that the UI has
    // changed successfully. Missing or false entries are reported.
    struct SettingCheck {
        std::string description;
        bool applied; // true if the setting was actually changed
    };
    std::vector<std::string> verify_settings(const std::vector<SettingCheck>& checks,
                                             const BenchmarkResult& after) const;

    // Based on comparison, suggest rollbacks for regressions.
    std::vector<std::string> rollback_recommendations(const BenchmarkResult& before,
                                                      const BenchmarkResult& after,
                                                      const std::vector<SettingCheck>& checks) const;

private:
    // Snapshot of live system usage while the benchmark runs. Backed by a
    // HardwareMonitor instance so CPU/GPU/RAM/disk figures come from the same
    // real PDH sampling the dashboard uses.
    struct SysSnapshot {
        double cpu_percent;
        double gpu_percent;
        double ram_mb;
        double disk_mb_s;
    };
    SysSnapshot sample_system();

    // Real frame workload: each iteration performs a fixed CPU workload and is
    // timed with the high-resolution clock, so the reported FPS reflects the
    // machine's actual throughput rather than a hard-coded target.
    void run_render_loop(double duration_seconds, std::vector<FrameData>& out_frames);

    HardwareMonitor _monitor;
};
