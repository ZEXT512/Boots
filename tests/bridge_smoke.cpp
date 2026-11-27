// BOOTFPS bridge tests — exercise the real native command surface used by
// the WebView2 frontend. No test framework; the exit code is the result.
#include "UI/Bridge.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace {
    int failures = 0;

    void check(bool condition, const std::string& name) {
        if (condition) {
            std::cout << "[ PASS ] " << name << '\n';
        } else {
            std::cout << "[ FAIL ] " << name << '\n';
            ++failures;
        }
    }
}

int main() {
    Bridge bridge;

    auto call = [&bridge](const std::string& command, json args = json::object()) -> json {
        json request = {{"id", 1}, {"command", command}, {"args", args}};
        std::string response = bridge.handle(request.dump());
        try {
            return json::parse(response);
        } catch (const std::exception& error) {
            std::cout << "[ ERROR ] invalid JSON for " << command << ": " << error.what() << '\n';
            return json{{"ok", false}};
        }
    };

    auto info = call("system.info");
    check(info.value("ok", false), "system.info responds");
    check(info["data"].value("cores", 0u) > 0, "system.info reports logical cores");
    check(!info["data"].value("os", std::string{}).empty(), "system.info reports OS");
    check(!info["data"].value("cpu", std::string{}).empty(), "system.info reports CPU");

    // Give the hardware sampler one interval to collect real values.
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    auto metrics = call("metrics.sample");
    check(metrics.value("ok", false), "metrics.sample responds");
    check(metrics["data"].contains("cpu") && metrics["data"].contains("mem_percent"),
          "metrics.sample returns CPU and memory");
    check(metrics["data"].value("mem_total_mb", 0.0) > 0.0, "metrics.sample reports physical RAM");

    auto processes = call("process.list", {{"limit", 5}});
    check(processes.value("ok", false), "process.list responds");
    check(processes["data"].value("total", 0) > 0, "process.list finds processes");
    check(processes["data"]["processes"].is_array(), "process.list returns an array");
    if (processes["data"]["processes"].is_array() &&
        !processes["data"]["processes"].empty()) {
        const auto& first = processes["data"]["processes"].front();
        check(first.contains("pid") && first.contains("name") && first.contains("class"),
              "process entries carry pid/name/class");
    }

    auto registry = call("registry.scan");
    check(registry.value("ok", false), "registry.scan responds");
    check(registry["data"].value("count", 0) > 0, "registry.scan returns managed keys");

    auto power = call("power.list");
    check(power.value("ok", false), "power.list responds");
    check(power["data"]["plans"].is_array() && !power["data"]["plans"].empty(),
          "power.list returns schemes");

    auto diagnose = call("power.diagnose");
    check(diagnose.value("ok", false), "power.diagnose responds");
    check(diagnose["data"]["warnings"].is_array(), "power.diagnose returns warnings");

    auto emulators = call("emulator.detect");
    check(emulators.value("ok", false), "emulator.detect responds");
    check(emulators["data"]["instances"].is_array(), "emulator.detect returns an array");

    auto input = call("input.read");
    check(input.value("ok", false), "input.read responds");
    check(input["data"]["mouse"].contains("speed") &&
          input["data"]["keyboard"].contains("rate"),
          "input.read returns mouse and keyboard values");

    auto devices = call("input.devices");
    check(devices.value("ok", false), "input.devices responds");
    check(devices["data"]["devices"].is_array(), "input.devices returns an array");

    auto logs = call("logs.tail", {{"lines", 20}});
    check(logs.value("ok", false), "logs.tail responds");
    check(logs["data"]["lines"].is_array(), "logs.tail returns log lines");

    auto unknown = call("does.not.exist");
    check(!unknown.value("ok", true), "unknown command is rejected");

    bridge.stop();

    std::cout << "Failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
