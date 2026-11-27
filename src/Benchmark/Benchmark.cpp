#include "Benchmark.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <thread>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <ctime>
#include <nlohmann/json.hpp>

#pragma comment(lib, "pdh.lib")

using json = nlohmann::json;

Result<void> PerformanceBenchmark::initialize() {
    _monitor.start(250);
    Logger::instance().log(Logger::Level::Info, L"PerformanceBenchmark initialized.");
    return Result<void>::Ok();
}

Result<void> PerformanceBenchmark::shutdown() {
    _monitor.stop();
    Logger::instance().log(Logger::Level::Info, L"PerformanceBenchmark shutdown.");
    return Result<void>::Ok();
}

// ---------------------------------------------------------------------
// Live system usage – sourced from the same HardwareMonitor that powers the
// dashboard, so CPU/GPU/RAM/disk values are real PDH samples.
// ---------------------------------------------------------------------
PerformanceBenchmark::SysSnapshot PerformanceBenchmark::sample_system() {
    HardwareSnapshot snap = _monitor.get_latest();
    SysSnapshot out{};
    out.cpu_percent = snap.cpu_total_percent;
    out.gpu_percent = snap.gpu_util_percent >= 0.0 ? snap.gpu_util_percent : 0.0;
    out.ram_mb = static_cast<double>(snap.mem_used_bytes) / (1024.0 * 1024.0);
    out.disk_mb_s = (snap.disk_read_bytes_per_sec + snap.disk_write_bytes_per_sec) /
                    (1024.0 * 1024.0);
    return out;
}

// ---------------------------------------------------------------------
// Real frame workload – every iteration does a fixed amount of floating-point
// work and is timed with the high-resolution clock. There is no artificial
// sleep: the achieved frame rate depends on the machine and its current load,
// which is exactly what the before/after comparison needs.
// ---------------------------------------------------------------------
void PerformanceBenchmark::run_render_loop(double duration_seconds, std::vector<FrameData>& out_frames) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto previous = start;
    double sink = 0.0;

    while (true) {
        auto frame_start = clock::now();
        double elapsed = std::chrono::duration<double>(frame_start - start).count();
        if (elapsed >= duration_seconds) break;

        // ~120k iterations of mixed math keeps a frame in the low milliseconds
        // on a modern desktop while remaining fully deterministic.
        double accumulator = 0.0;
        for (int i = 0; i < 120000; ++i) {
            double x = static_cast<double>(i) * 0.001;
            accumulator += std::sin(x) * std::cos(x * 1.5) + std::sqrt(x + 1.0);
        }
        sink += accumulator;

        auto frame_end = clock::now();
        double frame_ms = std::chrono::duration<double, std::milli>(frame_end - previous).count();
        previous = frame_end;
        if (frame_ms > 0.0) {
            out_frames.push_back({1000.0 / frame_ms, frame_ms});
        }
    }

    // Prevent the optimiser from discarding the workload.
    static volatile double keep_alive = 0.0;
    keep_alive = sink;
}

Result<BenchmarkResult> PerformanceBenchmark::run(double duration_seconds) {
    BenchmarkResult result;
    result.timestamp = std::chrono::system_clock::now();

    // Let the sampler prime itself before the workload starts.
    SysSnapshot before = sample_system();

    // 1. Run the real workload to collect frame timings.
    run_render_loop(duration_seconds, result.frames);
    if (result.frames.empty()) {
        return Result<BenchmarkResult>::Err("Render loop produced no frames.");
    }

    // 2. Compute FPS stats.
    std::vector<double> fps_vals;
    fps_vals.reserve(result.frames.size());
    for (const auto& f : result.frames) fps_vals.push_back(f.fps);
    result.avg_fps = std::accumulate(fps_vals.begin(), fps_vals.end(), 0.0) / fps_vals.size();
    std::sort(fps_vals.begin(), fps_vals.end());
    size_t idx_1pct = static_cast<size_t>(std::floor(fps_vals.size() * 0.01));
    if (idx_1pct >= fps_vals.size()) idx_1pct = fps_vals.size() - 1;
    result.low_1_percent_fps = fps_vals[idx_1pct];
    double total_frame_time = 0.0;
    for (const auto& f : result.frames) total_frame_time += f.frame_time_ms;
    result.avg_frame_time_ms = total_frame_time / result.frames.size();

    // 3. Sample system usage across the run and report the average.
    SysSnapshot after = sample_system();
    result.cpu_usage_percent = (before.cpu_percent + after.cpu_percent) / 2.0;
    result.gpu_usage_percent = (before.gpu_percent + after.gpu_percent) / 2.0;
    result.ram_usage_mb = (before.ram_mb + after.ram_mb) / 2.0;
    result.disk_activity_mb_s = (before.disk_mb_s + after.disk_mb_s) / 2.0;
    return Result<BenchmarkResult>::Ok(result);
}

// ---------------------------------------------------------------------
// JSON report generation
// ---------------------------------------------------------------------
std::string PerformanceBenchmark::generate_json_report(const BenchmarkResult& before,
                                                       const BenchmarkResult& after) const {
    json j;
    auto to_iso = [](const std::chrono::system_clock::time_point& tp){
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm; localtime_s(&tm, &t);
        char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        return std::string(buf);
    };
    j["baseline"] = {
        {"timestamp", to_iso(before.timestamp)},
        {"avg_fps", before.avg_fps},
        {"low_1_percent_fps", before.low_1_percent_fps},
        {"avg_frame_time_ms", before.avg_frame_time_ms},
        {"cpu_usage_percent", before.cpu_usage_percent},
        {"gpu_usage_percent", before.gpu_usage_percent},
        {"ram_usage_mb", before.ram_usage_mb},
        {"disk_activity_mb_s", before.disk_activity_mb_s}
    };
    j["after"] = {
        {"timestamp", to_iso(after.timestamp)},
        {"avg_fps", after.avg_fps},
        {"low_1_percent_fps", after.low_1_percent_fps},
        {"avg_frame_time_ms", after.avg_frame_time_ms},
        {"cpu_usage_percent", after.cpu_usage_percent},
        {"gpu_usage_percent", after.gpu_usage_percent},
        {"ram_usage_mb", after.ram_usage_mb},
        {"disk_activity_mb_s", after.disk_activity_mb_s}
    };
    return j.dump(4);
}

// ---------------------------------------------------------------------
// Simple HTML report – uses inline CSS for portability.
// ---------------------------------------------------------------------
std::string PerformanceBenchmark::generate_html_report(const BenchmarkResult& before,
                                                       const BenchmarkResult& after) const {
    std::ostringstream out;
    out << "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>BOOTFPS Benchmark Report</title>";
    out << "<style>body{font-family:Arial,sans-serif;background:#050505;color:#F5F5F5;margin:20px;}";
    out << "table{border-collapse:collapse;width:80%;margin:auto;}th,td{border:1px solid #292929;padding:8px;text-align:center;}";
    out << "th{background:#101010;}";
    out << ".diff{background:#151515;}";
    out << "</style></head><body><h1>BOOTFPS Benchmark Report</h1>";
    out << "<table><tr><th>Metric</th><th>Baseline</th><th>After</th><th>Δ</th></tr>";
    auto fmt = [&](double v){ std::ostringstream ss; ss<<std::fixed<<std::setprecision(2)<<v; return ss.str(); };
    // Helper to compute delta with sign.
    auto delta_str = [&](double before, double after){ double d = after - before; char sign = (d>=0?'+':'-'); std::ostringstream ss; ss<<sign<<std::fixed<<std::setprecision(2)<<std::abs(d); return ss.str(); };
    out << "<tr><td>Average FPS</td><td>"<<fmt(before.avg_fps)<<"</td><td>"<<fmt(after.avg_fps)<<"</td><td class='diff'>"<<delta_str(before.avg_fps, after.avg_fps)<<"</td></tr>";
    out << "<tr><td>1% Low FPS</td><td>"<<fmt(before.low_1_percent_fps)<<"</td><td>"<<fmt(after.low_1_percent_fps)<<"</td><td class='diff'>"<<delta_str(before.low_1_percent_fps, after.low_1_percent_fps)<<"</td></tr>";
    out << "<tr><td>Avg Frame Time (ms)</td><td>"<<fmt(before.avg_frame_time_ms)<<"</td><td>"<<fmt(after.avg_frame_time_ms)<<"</td><td class='diff'>"<<delta_str(before.avg_frame_time_ms, after.avg_frame_time_ms)<<"</td></tr>";
    out << "<tr><td>CPU Usage (%)</td><td>"<<fmt(before.cpu_usage_percent)<<"</td><td>"<<fmt(after.cpu_usage_percent)<<"</td><td class='diff'>"<<delta_str(before.cpu_usage_percent, after.cpu_usage_percent)<<"</td></tr>";
    out << "<tr><td>GPU Usage (%)</td><td>"<<fmt(before.gpu_usage_percent)<<"</td><td>"<<fmt(after.gpu_usage_percent)<<"</td><td class='diff'>"<<delta_str(before.gpu_usage_percent, after.gpu_usage_percent)<<"</td></tr>";
    out << "<tr><td>RAM Usage (MB)</td><td>"<<fmt(before.ram_usage_mb)<<"</td><td>"<<fmt(after.ram_usage_mb)<<"</td><td class='diff'>"<<delta_str(before.ram_usage_mb, after.ram_usage_mb)<<"</td></tr>";
    out << "<tr><td>Disk Activity (MB/s)</td><td>"<<fmt(before.disk_activity_mb_s)<<"</td><td>"<<fmt(after.disk_activity_mb_s)<<"</td><td class='diff'>"<<delta_str(before.disk_activity_mb_s, after.disk_activity_mb_s)<<"</td></tr>";
    out << "</table></body></html>";
    return out.str();
}

// ---------------------------------------------------------------------
// Comparison – human readable list of findings.
// ---------------------------------------------------------------------
std::vector<std::string> PerformanceBenchmark::compare(const BenchmarkResult& before,
                                                       const BenchmarkResult& after) const {
    std::vector<std::string> out;
    auto add = [&](const std::string& name, double b, double a, bool higher_is_better){
        double diff = a - b;
        if (std::abs(diff) < 0.01) return; // ignore noise
        std::ostringstream ss;
        ss << name << ": ";
        if (higher_is_better) {
            if (diff > 0) ss << "Improved by +" << diff << " (" << a << ")";
            else ss << "Degraded by " << diff << " (" << a << ")";
        } else {
            if (diff < 0) ss << "Improved by " << -diff << " (" << a << ")";
            else ss << "Degraded by +" << diff << " (" << a << ")";
        }
        out.push_back(ss.str());
    };
    add("Average FPS", before.avg_fps, after.avg_fps, true);
    add("1% Low FPS", before.low_1_percent_fps, after.low_1_percent_fps, true);
    add("Avg Frame Time (ms)", before.avg_frame_time_ms, after.avg_frame_time_ms, false);
    add("CPU Usage (%)", before.cpu_usage_percent, after.cpu_usage_percent, false);
    add("GPU Usage (%)", before.gpu_usage_percent, after.gpu_usage_percent, false);
    add("RAM Usage (MB)", before.ram_usage_mb, after.ram_usage_mb, false);
    add("Disk Activity (MB/s)", before.disk_activity_mb_s, after.disk_activity_mb_s, false);
    return out;
}

// ---------------------------------------------------------------------
// Settings verification – simple check that each SettingCheck reported as applied.
// ---------------------------------------------------------------------
std::vector<std::string> PerformanceBenchmark::verify_settings(const std::vector<SettingCheck>& checks,
                                                                const BenchmarkResult& after) const {
    std::vector<std::string> issues;
    for (const auto& ck : checks) {
        if (!ck.applied) {
            issues.push_back("Setting not applied: " + ck.description);
        }
    }
    return issues;
}

// ---------------------------------------------------------------------
// Rollback recommendations – if any metric worsened, suggest reverting settings.
// ---------------------------------------------------------------------
std::vector<std::string> PerformanceBenchmark::rollback_recommendations(const BenchmarkResult& before,
                                                                      const BenchmarkResult& after,
                                                                      const std::vector<SettingCheck>& checks) const {
    std::vector<std::string> recs;
    // Simple heuristic: if average FPS dropped >2% or 1% low FPS dropped >5%, suggest rollback.
    if (after.avg_fps < before.avg_fps * 0.98) {
        recs.push_back("Average FPS decreased noticeably – consider reverting recent optimizations.");
    }
    if (after.low_1_percent_fps < before.low_1_percent_fps * 0.95) {
        recs.push_back("1% low FPS degraded – rollback settings that affect throttle limits.");
    }
    // Suggest which settings may be responsible – list all that were applied.
    if (!recs.empty()) {
        recs.push_back("Potential culprits (applied settings):");
        for (const auto& ck : checks) {
            if (ck.applied) recs.push_back(" - " + ck.description);
        }
    }
    return recs;
}

