// BOOTFPS — CoreEngine implementation.
#include "CoreEngine.h"

#include "../common/Logger.h"
#include "../Benchmark/Benchmark.h"
#include "../EmulatorOptimizer/EmulatorOptimizer.h"
#include "../HardwareMonitor/HardwareMonitor.h"
#include "../InputLatencyOptimizer/InputLatency.h"
#include "../PowerOptimizer/PowerOptimizer.h"
#include "../ProcessManager/ProcessManager.h"
#include "../RegistryOptimizer/RegistryOptimizer.h"
#include "../WindowsSystemAnalyzer/SystemInfoProvider.h"

#include <mutex>
#include <vector>

struct CoreEngine::Impl {
    ProcessManager processes;
    RegistryOptimizer registry;
    PowerOptimizer power;
    EmulatorOptimizer emulators;
    InputLatencyOptimizer input;
    PerformanceBenchmark benchmark;
    HardwareMonitor hardware;
    WindowsSystemAnalyzer::SystemInfoProvider system;

    std::mutex mutex;
    bool initialized = false;
};

CoreEngine::CoreEngine() : _impl(std::make_unique<Impl>()) {}
CoreEngine::~CoreEngine() = default;

CoreEngine& CoreEngine::instance() {
    static CoreEngine engine;
    return engine;
}

Result<void> CoreEngine::initialize() {
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (_impl->initialized) return Result<void>::Ok();

    // Every module is initialised; a failure is logged but does not prevent
    // the rest of the application from starting (e.g. non-elevated runs
    // cannot touch HKLM-backed optimisers).
    struct Step {
        const char* name;
        Result<void> result;
    };
    std::vector<Step> steps;
    steps.push_back({"SystemInfoProvider", _impl->system.initialize()});
    steps.push_back({"ProcessManager", _impl->processes.initialize()});
    steps.push_back({"RegistryOptimizer", _impl->registry.initialize()});
    steps.push_back({"PowerOptimizer", _impl->power.initialize()});
    steps.push_back({"EmulatorOptimizer", _impl->emulators.initialize()});
    steps.push_back({"InputLatencyOptimizer", _impl->input.initialize()});
    steps.push_back({"PerformanceBenchmark", _impl->benchmark.initialize()});

    bool any_failure = false;
    for (const auto& step : steps) {
        if (step.result.is_err()) {
            any_failure = true;
            std::string message = std::string(step.name) + " init failed: " + step.result.error();
            Logger::instance().log(Logger::Level::Warn,
                                   std::wstring(message.begin(), message.end()));
        }
    }

    // Start the live sampler so the dashboard has data immediately.
    _impl->hardware.start(1000);

    _impl->initialized = true;
    Logger::instance().log(Logger::Level::Info,
                           any_failure ? L"CoreEngine initialized with warnings."
                                       : L"CoreEngine initialized.");
    return Result<void>::Ok();
}

Result<void> CoreEngine::shutdown() {
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (!_impl->initialized) return Result<void>::Ok();

    _impl->hardware.stop();
    _impl->input.shutdown();
    _impl->benchmark.shutdown();
    _impl->emulators.shutdown();
    _impl->power.shutdown();
    _impl->registry.shutdown();
    _impl->processes.shutdown();
    _impl->system.shutdown();

    _impl->initialized = false;
    Logger::instance().log(Logger::Level::Info, L"CoreEngine shutdown complete.");
    return Result<void>::Ok();
}

bool CoreEngine::is_initialized() const {
    return _impl->initialized;
}

Result<void> CoreEngine::apply_change(const std::string& description,
                                      std::function<Result<void>()> change_func) {
    Logger::instance().log(Logger::Level::Info,
                           L"Applying change: " + std::wstring(description.begin(), description.end()));
    if (!change_func) return Result<void>::Err("No change function supplied.");
    Result<void> result = change_func();
    if (result.is_err()) {
        std::string message = "Change failed (" + description + "): " + result.error();
        Logger::instance().log(Logger::Level::Error,
                               std::wstring(message.begin(), message.end()));
        return result;
    }
    Logger::instance().log(Logger::Level::Info,
                           L"Change applied: " + std::wstring(description.begin(), description.end()));
    return Result<void>::Ok();
}

ProcessManager& CoreEngine::processes() { return _impl->processes; }
RegistryOptimizer& CoreEngine::registry() { return _impl->registry; }
PowerOptimizer& CoreEngine::power() { return _impl->power; }
EmulatorOptimizer& CoreEngine::emulators() { return _impl->emulators; }
InputLatencyOptimizer& CoreEngine::input() { return _impl->input; }
PerformanceBenchmark& CoreEngine::benchmark() { return _impl->benchmark; }
HardwareMonitor& CoreEngine::hardware() { return _impl->hardware; }
WindowsSystemAnalyzer::SystemInfoProvider& CoreEngine::system() { return _impl->system; }
