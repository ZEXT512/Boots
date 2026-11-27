#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../common/Result.h"

class ProcessManager;
class RegistryOptimizer;
class PowerOptimizer;
class EmulatorOptimizer;
class InputLatencyOptimizer;
class PerformanceBenchmark;
class HardwareMonitor;
namespace WindowsSystemAnalyzer { class SystemInfoProvider; }

/**
 * CoreEngine — the single entry point used by the UI bridge.
 *
 * It owns one instance of every BOOTFPS module, initialises them once and
 * exposes them to the frontend. Long-running work stays inside the modules;
 * the bridge decides which thread to call from.
 */
class CoreEngine {
public:
    static CoreEngine& instance();
    ~CoreEngine();

    Result<void> initialize();
    Result<void> shutdown();
    bool is_initialized() const;

    // Called by the UI when the user confirms a change. Logs the description
    // and the outcome so the activity log mirrors every write.
    Result<void> apply_change(const std::string& description,
                              std::function<Result<void>()> change_func);

    ProcessManager& processes();
    RegistryOptimizer& registry();
    PowerOptimizer& power();
    EmulatorOptimizer& emulators();
    InputLatencyOptimizer& input();
    PerformanceBenchmark& benchmark();
    HardwareMonitor& hardware();
    WindowsSystemAnalyzer::SystemInfoProvider& system();

private:
    CoreEngine();
    CoreEngine(const CoreEngine&) = delete;
    CoreEngine& operator=(const CoreEngine&) = delete;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};
