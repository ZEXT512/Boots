// BOOTFPS smoke tests — verifies the built modules and the preserved
// Cyber Sigilism report design. Uses no test framework by design.
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "common/Result.h"
#include "Benchmark/Benchmark.h"

namespace {
    int failures = 0;

    void check(bool condition, const char* name) {
        if (condition) {
            std::cout << "[ PASS ] " << name << '\n';
        } else {
            std::cout << "[ FAIL ] " << name << '\n';
            ++failures;
        }
    }
}

int main() {
    // Result<T> / Result<void> error handling used by every module.
    auto ok = Result<int>::Ok(42);
    check(ok.is_ok() && !ok.is_err() && ok.unwrap() == 42, "Result<T>::Ok");

    auto err = Result<int>::Err("boom");
    check(err.is_err() && !err.is_ok() && err.error() == "boom", "Result<T>::Err");

    auto void_ok = Result<void>::Ok();
    check(void_ok.is_ok() && !void_ok.is_err(), "Result<void>::Ok");

    auto void_err = Result<void>::Err("bad");
    check(void_err.is_err() && void_err.error() == "bad", "Result<void>::Err");

    // Benchmark reports — the Cyber Sigilism palette must survive the CMake repair.
    BenchmarkResult before{};
    BenchmarkResult after{};
    before.timestamp = std::chrono::system_clock::now();
    after.timestamp = before.timestamp;
    before.avg_fps = 100.0;
    before.low_1_percent_fps = 90.0;
    before.avg_frame_time_ms = 10.0;
    after.avg_fps = 140.0;
    after.low_1_percent_fps = 130.0;
    after.avg_frame_time_ms = 7.14;

    PerformanceBenchmark benchmark;
    const std::string json = benchmark.generate_json_report(before, after);
    check(json.find("\"baseline\"") != std::string::npos &&
          json.find("\"after\"") != std::string::npos &&
          json.find("\"avg_fps\"") != std::string::npos, "JSON report structure");

    const std::string html = benchmark.generate_html_report(before, after);
    check(html.find("#050505") != std::string::npos &&
          html.find("#F5F5F5") != std::string::npos &&
          html.find("#292929") != std::string::npos &&
          html.find("#151515") != std::string::npos, "Cyber Sigilism HTML palette");

    const auto findings = benchmark.compare(before, after);
    check(!findings.empty() && findings.front().find("Improved") != std::string::npos,
          "Benchmark comparison");

    const std::vector<PerformanceBenchmark::SettingCheck> checks{
        {"smoke setting", true}
    };
    check(benchmark.verify_settings(checks, after).empty(), "Applied settings verification");

    const auto recommendations = benchmark.rollback_recommendations(after, before, checks);
    check(!recommendations.empty(), "Rollback recommendation on regression");

    // PdhQueryWrapper (implemented in Benchmark.cpp) — live PDH smoke check.
    // Counter names are localized on some Windows installs; a failed add is
    // reported but does not fail the suite.
    PdhQueryWrapper query;
    const auto added = query.add_counter(L"\\Processor(_Total)\\% Processor Time");
    if (added.is_err()) {
        std::cout << "[ INFO ] PDH counter unavailable on this host: " << added.error() << '\n';
    } else {
        check(query.counter_count() == 1, "PDH wrapper counter registration");
        const auto collected = query.collect();
        check(!collected.is_err(), "PDH wrapper collection");
    }

    std::cout << "Failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
