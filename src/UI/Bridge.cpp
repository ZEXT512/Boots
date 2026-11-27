// BOOTFPS — native bridge implementation.
//
// One JSON command in, one JSON envelope out. Every command calls a real
// module method; nothing here is a placeholder.
#include "Bridge.h"

#include <nlohmann/json.hpp>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "../CoreEngine/CoreEngine.h"
#include "../EmulatorOptimizer/EmulatorOptimizer.h"
#include "../HardwareMonitor/HardwareMonitor.h"
#include "../InputLatencyOptimizer/InputLatency.h"
#include "../PowerOptimizer/PowerOptimizer.h"
#include "../ProcessManager/ProcessManager.h"
#include "../RegistryOptimizer/RegistryOptimizer.h"
#include "../WindowsSystemAnalyzer/SystemInfoProvider.h"

using json = nlohmann::json;

namespace {

// ── Encoding helpers ─────────────────────────────────────────────────────
std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

std::string narrow_ascii(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) out.push_back(c <= 0x7F ? static_cast<char>(c) : '?');
    return out;
}

std::string timestamp_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm);
    return buffer;
}

std::string program_data() {
    const char* value = std::getenv("ProgramData");
    return value ? value : "C:\\ProgramData";
}

std::string guid_to_string(const GUID& guid) {
    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, 64);
    return to_utf8(buffer);
}

std::string classify_name(ProcessManager::Classification value) {
    switch (value) {
        case ProcessManager::Classification::WindowsCritical: return "windows-critical";
        case ProcessManager::Classification::SecurityCritical: return "security-critical";
        case ProcessManager::Classification::DriverRelated: return "driver";
        case ProcessManager::Classification::UserApplication: return "user-app";
        case ProcessManager::Classification::BackgroundApplication: return "background";
        case ProcessManager::Classification::OptionalStartup: return "startup";
        default: return "unknown";
    }
}

std::string reg_type_name(RegDataType type) {
    switch (type) {
        case RegDataType::DWord: return "DWORD";
        case RegDataType::QWord: return "QWORD";
        case RegDataType::String: return "REG_SZ";
        case RegDataType::ExpandString: return "REG_EXPAND_SZ";
        case RegDataType::Binary: return "REG_BINARY";
        default: return "UNKNOWN";
    }
}

std::string risk_name(RiskLevel risk) {
    switch (risk) {
        case RiskLevel::Low: return "low";
        case RiskLevel::Medium: return "medium";
        case RiskLevel::High: return "high";
        case RiskLevel::Critical: return "critical";
    }
    return "unknown";
}

std::string root_name(HKEY root) {
    if (root == HKEY_CURRENT_USER) return "HKEY_CURRENT_USER";
    if (root == HKEY_LOCAL_MACHINE) return "HKEY_LOCAL_MACHINE";
    return "UNKNOWN";
}

json benchmark_to_json(const BenchmarkResult& result) {
    json value;
    value["avg_fps"] = result.avg_fps;
    value["low_1_percent_fps"] = result.low_1_percent_fps;
    value["avg_frame_time_ms"] = result.avg_frame_time_ms;
    value["cpu_usage_percent"] = result.cpu_usage_percent;
    value["gpu_usage_percent"] = result.gpu_usage_percent;
    value["ram_usage_mb"] = result.ram_usage_mb;
    value["disk_activity_mb_s"] = result.disk_activity_mb_s;
    value["frames"] = result.frames.size();
    std::time_t when = std::chrono::system_clock::to_time_t(result.timestamp);
    std::tm tm{};
    localtime_s(&tm, &when);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    value["timestamp"] = buffer;
    return value;
}

json bytes_to_mb(unsigned long long bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename T>
Result<T> expect_ok(Result<T>&& result) {
    if (result.is_err()) fail(result.error());
    return std::move(result);
}

void expect_void(Result<void>&& result) {
    if (result.is_err()) fail(result.error());
}

} // namespace

// ── Construction / threading ─────────────────────────────────────────────
Bridge::Bridge() {
    CoreEngine::instance().initialize();
}

Bridge::~Bridge() {
    stop();
    CoreEngine::instance().shutdown();
}

void Bridge::attach(HWND hwnd) {
    _hwnd = hwnd;
}

void Bridge::start() {
    if (_running) return;
    _running = true;
    _worker = std::thread(&Bridge::worker_loop, this);
}

void Bridge::stop() {
    if (!_running.exchange(false)) {
        if (_worker.joinable()) _worker.join();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        _queue.clear();
    }
    _queue_cv.notify_all();
    if (_worker.joinable()) _worker.join();
}

void Bridge::submit(const std::string& request) {
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        if (!_running) return;
        _queue.push_back(request);
    }
    _queue_cv.notify_one();
}

void Bridge::worker_loop() {
    for (;;) {
        std::string request;
        {
            std::unique_lock<std::mutex> lock(_queue_mutex);
            _queue_cv.wait(lock, [this] { return !_running || !_queue.empty(); });
            if (!_running && _queue.empty()) break;
            request = std::move(_queue.front());
            _queue.pop_front();
        }
        deliver(handle(request));
    }
}

void Bridge::deliver(const std::string& payload) {
    if (!_hwnd) return;
    auto* copy = new std::string(payload);
    if (!PostMessageW(_hwnd, WM_BOOTFPS_BRIDGE_RESULT, 0,
                      reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

// ── Command dispatch ─────────────────────────────────────────────────────
std::string Bridge::handle(const std::string& request) {
    json envelope;
    json id = nullptr;
    std::string command;

    try {
        json parsed = json::parse(request);
        if (parsed.contains("id")) id = parsed["id"];
        command = parsed.value("command", std::string{});
        json args = parsed.contains("args") && parsed["args"].is_object()
                        ? parsed["args"]
                        : json::object();

        CoreEngine& engine = CoreEngine::instance();
        json data;

        if (command == "ping") {
            data = {
                {"pong", true},
                {"app", "BOOTFPS"},
                {"bridge", "online"},
                {"admin", engine.system().is_admin()}
            };
        }
        // ── System ───────────────────────────────────────────────────────
        else if (command == "system.info") {
            auto& system = engine.system();
            data = {
                {"os", system.os_version()},
                {"hostname", system.hostname()},
                {"user", system.user_name()},
                {"admin", system.is_admin()},
                {"cpu", system.cpu_name()},
                {"cores", system.logical_cores()},
                {"ram_total_mb", system.total_ram_mb()},
                {"uptime_seconds", system.uptime_seconds()},
                {"engine_ready", engine.is_initialized()},
                {"version", "1.0.0"},
                {"modules", json::array({
                    "SystemInfoProvider", "HardwareMonitor", "ProcessManager",
                    "RegistryOptimizer", "PowerOptimizer", "EmulatorOptimizer",
                    "InputLatencyOptimizer", "PerformanceBenchmark"
                })}
            };
        }
        // ── Live metrics ─────────────────────────────────────────────────
        else if (command == "metrics.sample") {
            auto& hardware = engine.hardware();
            HardwareSnapshot snap = hardware.get_latest();
            data = {
                {"cpu", snap.cpu_total_percent},
                {"per_core", snap.cpu_per_core_percent},
                {"mem_total_mb", bytes_to_mb(snap.mem_total_bytes)},
                {"mem_used_mb", bytes_to_mb(snap.mem_used_bytes)},
                {"mem_percent", snap.mem_pressure_percent},
                {"gpu", snap.gpu_util_percent >= 0.0 ? json(snap.gpu_util_percent) : json(nullptr)},
                {"gpu_engine", snap.gpu_engine_util_percent >= 0.0 ? json(snap.gpu_engine_util_percent) : json(nullptr)},
                {"disk_read_mb_s", snap.disk_read_bytes_per_sec / (1024.0 * 1024.0)},
                {"disk_write_mb_s", snap.disk_write_bytes_per_sec / (1024.0 * 1024.0)},
                {"disk_active_percent", snap.disk_active_time_percent >= 0.0 ? json(snap.disk_active_time_percent) : json(nullptr)},
                {"warnings", hardware.analyze_performance(snap)}
            };
        }
        // ── Processes ────────────────────────────────────────────────────
        else if (command == "process.list") {
            auto& processes = engine.processes();
            expect_void(processes.refresh());

            std::vector<ProcessInfo> snapshot = processes.get_processes();
            const std::string sort = args.value("sort", std::string("cpu"));
            if (sort == "mem") {
                std::sort(snapshot.begin(), snapshot.end(),
                          [](const ProcessInfo& a, const ProcessInfo& b) {
                              return a.memory_bytes > b.memory_bytes;
                          });
            } else if (sort == "name") {
                std::sort(snapshot.begin(), snapshot.end(),
                          [](const ProcessInfo& a, const ProcessInfo& b) {
                              std::wstring left = a.name, right = b.name;
                              std::transform(left.begin(), left.end(), left.begin(), ::towlower);
                              std::transform(right.begin(), right.end(), right.begin(), ::towlower);
                              return left < right;
                          });
            } else {
                std::sort(snapshot.begin(), snapshot.end(),
                          [](const ProcessInfo& a, const ProcessInfo& b) {
                              return a.cpu_percent > b.cpu_percent;
                          });
            }

            int limit = args.value("limit", 250);
            limit = std::clamp(limit, 1, 2000);

            json list = json::array();
            for (int i = 0; i < limit && i < static_cast<int>(snapshot.size()); ++i) {
                const ProcessInfo& info = snapshot[static_cast<size_t>(i)];
                list.push_back({
                    {"pid", info.pid},
                    {"name", to_utf8(info.name)},
                    {"cpu", info.cpu_percent},
                    {"mem_mb", static_cast<double>(info.memory_bytes) / (1024.0 * 1024.0)},
                    {"class", classify_name(processes.classify(info))},
                    {"whitelisted", processes.is_whitelisted(info.name)}
                });
            }
            data = {
                {"total", snapshot.size()},
                {"shown", list.size()},
                {"processes", list}
            };
        }
        else if (command == "process.kill") {
            DWORD pid = static_cast<DWORD>(args.value("pid", 0));
            if (pid == 0) fail("pid is required");
            auto result = expect_ok(engine.processes().terminate_process(pid));
            data = {{"message", result.unwrap()}};
        }
        else if (command == "process.whitelist") {
            const std::string action = args.value("action", std::string("add"));
            const std::wstring name = to_wide(args.value("name", std::string{}));
            if (name.empty()) fail("name is required");
            if (action == "add") {
                expect_void(engine.processes().add_to_whitelist(name));
            } else if (action == "remove") {
                expect_void(engine.processes().remove_from_whitelist(name));
            } else {
                fail("unknown whitelist action");
            }
            data = {{"message", action == "add" ? "Added to whitelist." : "Removed from whitelist."}};
        }
        // ── Registry ─────────────────────────────────────────────────────
        else if (command == "registry.scan") {
            auto& registry = engine.registry();
            expect_void(registry.scan());
            json entries = json::array();
            const auto& list = registry.get_entries();
            for (size_t i = 0; i < list.size(); ++i) {
                const RegistryEntry& entry = list[i];
                entries.push_back({
                    {"index", i},
                    {"root", root_name(entry.root)},
                    {"subkey", to_utf8(entry.subkey)},
                    {"value", to_utf8(entry.value_name)},
                    {"type", reg_type_name(entry.type)},
                    {"current", entry.current_str ? json(to_utf8(*entry.current_str)) : json(nullptr)},
                    {"proposed", entry.proposed_str ? json(to_utf8(*entry.proposed_str)) : json(nullptr)},
                    {"modified", entry.is_modified},
                    {"valid", entry.is_valid},
                    {"risk", risk_name(entry.risk)},
                    {"explanation", to_utf8(entry.explanation)},
                    {"compatibility", to_utf8(entry.compatibility)}
                });
            }
            data = {{"entries", entries}, {"count", entries.size()}};
        }
        else if (command == "registry.apply") {
            auto& registry = engine.registry();
            const auto& list = registry.get_entries();
            int index = args.value("index", -1);
            if (index < 0 || index >= static_cast<int>(list.size())) fail("registry index out of range");
            RegistryEntry entry = list[static_cast<size_t>(index)];

            std::string message;
            auto change = engine.apply_change(
                "registry " + to_utf8(entry.subkey) + "\\" + to_utf8(entry.value_name),
                [&]() -> Result<void> {
                    auto result = registry.apply_change(entry);
                    if (result.is_err()) return Result<void>::Err(result.error());
                    message = result.unwrap();
                    return Result<void>::Ok();
                });
            if (change.is_err()) fail(change.error());
            registry.scan();
            data = {{"message", message}};
        }
        else if (command == "registry.restore") {
            auto& registry = engine.registry();
            const auto& list = registry.get_entries();
            int index = args.value("index", -1);
            if (index < 0 || index >= static_cast<int>(list.size())) fail("registry index out of range");
            RegistryEntry entry = list[static_cast<size_t>(index)];

            std::string message;
            auto change = engine.apply_change(
                "restore registry " + to_utf8(entry.subkey) + "\\" + to_utf8(entry.value_name),
                [&]() -> Result<void> {
                    auto result = registry.restore_entry(entry);
                    if (result.is_err()) return Result<void>::Err(result.error());
                    message = result.unwrap();
                    return Result<void>::Ok();
                });
            if (change.is_err()) fail(change.error());
            registry.scan();
            data = {{"message", message}};
        }
        else if (command == "registry.backup") {
            expect_void(engine.registry().backup_profile());
            data = {{"message", "Registry profile backup written to ProgramData\\BOOTFPS\\registry_backups."}};
        }
        // ── Power ────────────────────────────────────────────────────────
        else if (command == "power.list") {
            auto& power = engine.power();
            auto plans = expect_ok(power.list_available_plans());
            GUID active{};
            auto current = power.get_current_plan();
            if (current.is_ok()) active = current.unwrap().guid;

            json list = json::array();
            for (const PowerPlan& plan : plans.unwrap()) {
                list.push_back({
                    {"guid", guid_to_string(plan.guid)},
                    {"name", to_utf8(plan.name)},
                    {"active", IsEqualGUID(plan.guid, active) != 0}
                });
            }
            data = {{"plans", list}};
        }
        else if (command == "power.set") {
            const std::string guid_text = args.value("guid", std::string{});
            if (guid_text.empty()) fail("guid is required");
            GUID guid{};
            if (FAILED(CLSIDFromString(to_wide(guid_text).c_str(), &guid))) {
                fail("invalid GUID");
            }
            PowerPlan plan{};
            plan.guid = guid;
            plan.is_active = true;
            expect_void(engine.power().set_plan(plan));
            data = {{"message", "Power scheme activated."}};
        }
        else if (command == "power.profile") {
            const std::string name = args.value("profile", std::string("Balanced"));
            PowerProfile profile = PowerProfile::Balanced;
            if (name == "Performance") profile = PowerProfile::Performance;
            else if (name == "Emulator") profile = PowerProfile::Emulator;
            else if (name == "Streaming") profile = PowerProfile::Streaming;
            else if (name == "Custom") profile = PowerProfile::Custom;
            expect_void(engine.power().apply_profile(profile));
            data = {{"message", "Power profile applied: " + name}};
        }
        else if (command == "power.diagnose") {
            data = {{"warnings", engine.power().diagnose()}};
        }
        else if (command == "power.restore") {
            expect_void(engine.power().restore_original());
            data = {{"message", "Original power scheme restored."}};
        }
        // ── Emulator ─────────────────────────────────────────────────────
        else if (command == "emulator.detect") {
            auto& emulators = engine.emulators();
            auto installed = expect_ok(emulators.detect_installed());
            auto running = expect_ok(emulators.detect_running(installed.unwrap()));

            json list = json::array();
            for (auto instance : running.unwrap()) {
                std::string config_error;
                auto config = emulators.read_configuration(instance);
                if (config.is_err()) config_error = config.error();

                list.push_back({
                    {"name", to_utf8(instance.name)},
                    {"version", to_utf8(instance.version)},
                    {"install_dir", to_utf8(instance.install_dir)},
                    {"pid", instance.pid},
                    {"running", instance.is_running},
                    {"cpu_cores", instance.configured_cpu_cores},
                    {"ram_mb", instance.configured_ram_mb},
                    {"rendering_mode", to_utf8(instance.rendering_mode)},
                    {"graphics_backend", to_utf8(instance.graphics_backend)},
                    {"config_ok", config_error.empty()},
                    {"config_error", config_error}
                });
            }
            data = {{"instances", list}, {"count", list.size()}};
        }
        else if (command == "emulator.profile" || command == "emulator.backup" ||
                 command == "emulator.restore") {
            auto& emulators = engine.emulators();
            auto installed = expect_ok(emulators.detect_installed());
            auto running = expect_ok(emulators.detect_running(installed.unwrap()));
            auto instances = running.unwrap();
            int index = args.value("index", -1);
            if (index < 0 || index >= static_cast<int>(instances.size())) {
                fail("emulator index out of range");
            }
            EmulatorInstance instance = instances[static_cast<size_t>(index)];

            if (command == "emulator.backup") {
                expect_void(emulators.backup_configuration(instance));
                data = {{"message", "Configuration backup created."}};
            } else if (command == "emulator.restore") {
                expect_void(emulators.restore_latest_backup(instance));
                data = {{"message", "Latest configuration backup restored."}};
            } else {
                const std::string name = args.value("profile", std::string("CompetitiveGaming"));
                EmulatorProfile profile = EmulatorProfile::CompetitiveGaming;
                if (name == "LowEndPC") profile = EmulatorProfile::LowEndPC;
                else if (name == "Streaming") profile = EmulatorProfile::Streaming;
                else if (name == "Custom") profile = EmulatorProfile::Custom;

                expect_void(emulators.backup_configuration(instance));
                expect_void(emulators.apply_profile(instance, profile));
                data = {{"message", "Emulator profile applied: " + name}};
            }
        }
        // ── Input ────────────────────────────────────────────────────────
        else if (command == "input.read") {
            std::lock_guard<std::mutex> lock(_input_mutex);
            auto& input = engine.input();
            auto mouse = expect_ok(input.read_mouse_config());
            auto keyboard = expect_ok(input.read_keyboard_config());
            data = {
                {"mouse", {
                    {"speed", mouse.unwrap().pointer_speed},
                    {"accel", mouse.unwrap().enhance_pointer_precision}
                }},
                {"keyboard", {
                    {"delay", keyboard.unwrap().repeat_delay},
                    {"rate", keyboard.unwrap().repeat_rate}
                }}
            };
        }
        else if (command == "input.mouse") {
            std::lock_guard<std::mutex> lock(_input_mutex);
            MouseConfig config{};
            config.pointer_speed = std::clamp(args.value("speed", 10), 1, 20);
            config.enhance_pointer_precision = args.value("accel", false);
            expect_void(engine.input().apply_mouse_profile(config));
            data = {{"message", "Mouse settings applied."}};
        }
        else if (command == "input.keyboard") {
            std::lock_guard<std::mutex> lock(_input_mutex);
            KeyboardConfig config{};
            config.repeat_delay = std::clamp(args.value("delay", 1), 0, 3);
            config.repeat_rate = std::clamp(args.value("rate", 20), 0, 31);
            expect_void(engine.input().apply_keyboard_profile(config));
            data = {{"message", "Keyboard settings applied."}};
        }
        else if (command == "input.profile") {
            std::lock_guard<std::mutex> lock(_input_mutex);
            const std::string name = args.value("profile", std::string("CompetitiveGaming"));
            InputLatencyOptimizer::Profile profile = InputLatencyOptimizer::Profile::CompetitiveGaming;
            if (name == "EmulatorGaming") profile = InputLatencyOptimizer::Profile::EmulatorGaming;
            else if (name == "StandardWindows") profile = InputLatencyOptimizer::Profile::StandardWindows;
            expect_void(engine.input().apply_profile(profile));
            data = {{"message", "Input profile applied: " + name}};
        }
        else if (command == "input.devices") {
            std::lock_guard<std::mutex> lock(_input_mutex);
            auto devices = expect_ok(engine.input().enumerate_usb_input_devices());
            json list = json::array();
            for (const USBDeviceInfo& device : devices.unwrap()) {
                list.push_back({
                    {"path", to_utf8(device.device_path)},
                    {"description", to_utf8(device.description)},
                    {"is_mouse", device.is_mouse},
                    {"is_keyboard", device.is_keyboard},
                    {"power_saving", device.power_saving_enabled}
                });
            }
            data = {{"devices", list}, {"count", list.size()}};
        }
        else if (command == "input.latency.start") {
            std::lock_guard<std::mutex> lock(_input_mutex);
            expect_void(engine.input().start_latency_measurement());
            data = {{"message", "Latency measurement started. Move the mouse or type, then stop."}};
        }
        else if (command == "input.latency.stop") {
            std::lock_guard<std::mutex> lock(_input_mutex);
            auto stats = expect_ok(engine.input().stop_latency_measurement());
            const LatencyStats& value = stats.unwrap();
            json samples = json::array();
            const size_t limit = std::min<size_t>(value.samples.size(), 500);
            for (size_t i = 0; i < limit; ++i) samples.push_back(value.samples[i].interval_ms);
            data = {
                {"min_ms", value.min_ms},
                {"max_ms", value.max_ms},
                {"average_ms", value.average_ms},
                {"stddev_ms", value.stddev_ms},
                {"count", value.samples.size()},
                {"samples", samples}
            };
        }
        // ── Benchmark ────────────────────────────────────────────────────
        else if (command == "benchmark.run") {
            double seconds = args.value("seconds", 5.0);
            seconds = std::clamp(seconds, 1.0, 30.0);
            auto result = expect_ok(engine.benchmark().run(seconds));
            auto value = std::make_unique<BenchmarkResult>(result.unwrap());

            std::string slot;
            if (!_baseline) {
                _baseline = std::move(value);
                slot = "baseline";
            } else {
                _after = std::move(value);
                slot = "after";
            }
            data = {
                {"slot", slot},
                {"result", benchmark_to_json(slot == "baseline" ? *_baseline : *_after)},
                {"has_baseline", static_cast<bool>(_baseline)},
                {"has_after", static_cast<bool>(_after)}
            };
        }
        else if (command == "benchmark.state") {
            data = {
                {"has_baseline", static_cast<bool>(_baseline)},
                {"has_after", static_cast<bool>(_after)},
                {"baseline", _baseline ? benchmark_to_json(*_baseline) : json(nullptr)},
                {"after", _after ? benchmark_to_json(*_after) : json(nullptr)}
            };
        }
        else if (command == "benchmark.compare") {
            if (!_baseline || !_after) fail("Run the benchmark twice (baseline + after) first.");
            auto& benchmark = engine.benchmark();
            data = {
                {"findings", benchmark.compare(*_baseline, *_after)},
                {"rollback", benchmark.rollback_recommendations(*_baseline, *_after, {})}
            };
        }
        else if (command == "benchmark.report") {
            if (!_baseline || !_after) fail("Run the benchmark twice (baseline + after) first.");
            auto& benchmark = engine.benchmark();
            const std::string jsonReport = benchmark.generate_json_report(*_baseline, *_after);
            const std::string htmlReport = benchmark.generate_html_report(*_baseline, *_after);

            const std::string directory = program_data() + "\\BOOTFPS\\reports";
            std::filesystem::create_directories(directory);
            const std::string stamp = timestamp_now();
            const std::string jsonPath = directory + "\\benchmark_" + stamp + ".json";
            const std::string htmlPath = directory + "\\benchmark_" + stamp + ".html";

            {
                std::ofstream out(jsonPath, std::ios::binary);
                if (!out) fail("could not write JSON report");
                out << jsonReport;
            }
            {
                std::ofstream out(htmlPath, std::ios::binary);
                if (!out) fail("could not write HTML report");
                out << htmlReport;
            }
            data = {
                {"json_path", jsonPath},
                {"html_path", htmlPath},
                {"html", htmlReport}
            };
        }
        else if (command == "benchmark.reset") {
            _baseline.reset();
            _after.reset();
            data = {{"message", "Benchmark history cleared."}};
        }
        // ── Logs ─────────────────────────────────────────────────────────
        else if (command == "logs.tail") {
            int lines = std::clamp(args.value("lines", 100), 1, 1000);
            std::ifstream in(program_data() + "\\BOOTFPS\\logs\\bootfps.log");
            std::deque<std::string> tail;
            std::string line;
            while (std::getline(in, line)) {
                tail.push_back(line);
                if (static_cast<int>(tail.size()) > lines) tail.pop_front();
            }
            data = {{"lines", json::array()}};
            for (const auto& item : tail) data["lines"].push_back(item);
        }
        else {
            fail("unknown command: " + command);
        }

        envelope = {{"id", id}, {"ok", true}, {"command", command}, {"data", data}};
    } catch (const std::exception& error) {
        envelope = {{"id", id}, {"ok", false}, {"command", command},
                    {"error", error.what()}};
    } catch (...) {
        envelope = {{"id", id}, {"ok", false}, {"command", command},
                    {"error", "unhandled native exception"}};
    }
    return envelope.dump();
}
