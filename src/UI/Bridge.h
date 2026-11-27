#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../Benchmark/Benchmark.h"

// Posted by the bridge worker to the UI thread with a heap-allocated
// std::string payload in lParam. The window procedure forwards it to WebView2.
constexpr UINT WM_BOOTFPS_BRIDGE_RESULT = WM_APP + 0x51;

/**
 * Bridge — translates WebView2 JSON messages into real BOOTFPS module calls.
 *
 * Every command returns a JSON envelope:
 *   { "id": <echoed>, "ok": true,  "command": "...", "data": { ... } }
 *   { "id": <echoed>, "ok": false, "command": "...", "error": "..." }
 *
 * Heavy commands (process scan, benchmark, registry scan) run on a dedicated
 * worker thread so the WebView2 UI thread stays responsive.
 */
class Bridge {
public:
    Bridge();
    ~Bridge();

    void attach(HWND hwnd);
    void start();
    void stop();

    // Queue a request for the worker thread.
    void submit(const std::string& request);

    // Execute synchronously and return the JSON envelope. Safe to call from
    // the UI thread for commands that must own the message loop (input hooks).
    std::string handle(const std::string& request);

private:
    void worker_loop();
    void deliver(const std::string& payload);

    HWND _hwnd = nullptr;
    std::thread _worker;
    std::mutex _queue_mutex;
    std::condition_variable _queue_cv;
    std::deque<std::string> _queue;
    std::atomic<bool> _running{false};

    // Input hooks are installed/removed on the UI thread while other input
    // commands run on the worker, so guard the shared module.
    std::mutex _input_mutex;

    // Benchmark history for the "before / after" comparison view.
    std::unique_ptr<BenchmarkResult> _baseline;
    std::unique_ptr<BenchmarkResult> _after;
};
