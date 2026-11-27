#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <filesystem>
#include <windows.h>
#include <winreg.h>
#include <setupapi.h>
#include <devguid.h>
#include <winusb.h>

#include "../common/Result.h"
#include "../common/Logger.h"

/**
 * Data structures for input devices and settings.
 */
struct MouseConfig {
    int pointer_speed;               // 1‑20 (SystemParametersInfo SPI_GETMOUSESPEED)
    bool enhance_pointer_precision; // SPI_GETMOUSEACCELERATE
};

struct KeyboardConfig {
    int repeat_delay; // 0‑3 (SystemParametersInfo SPI_GETKEYBOARDDELAY) * 250ms
    int repeat_rate;  // 0‑31 (SystemParametersInfo SPI_GETKEYBOARDSPEED) * 30‑120Hz
};

struct USBDeviceInfo {
    std::wstring device_path;   // Device interface path
    std::wstring description;   // Friendly name
    bool is_mouse;
    bool is_keyboard;
    bool power_saving_enabled;
};

struct LatencySample {
    double interval_ms; // Time between two input events
};

struct LatencyStats {
    double min_ms;
    double max_ms;
    double average_ms;
    double stddev_ms;
    std::vector<LatencySample> samples;
};

/**
 * InputLatencyOptimizer – analyzes and safely optimises mouse/keyboard/USB input latency.
 * All operations use documented Windows registry keys and system parameters.
 * No undocumented tweaks are applied.
 */
class InputLatencyOptimizer {
public:
    // Initialise/Shutdown – loads registry backup, sets up any required hooks.
    Result<void> initialize();
    Result<void> shutdown();

    // Mouse analysis
    Result<MouseConfig> read_mouse_config();
    Result<void> apply_mouse_profile(const MouseConfig& cfg);
    Result<void> restore_mouse_defaults();

    // Keyboard analysis
    Result<KeyboardConfig> read_keyboard_config();
    Result<void> apply_keyboard_profile(const KeyboardConfig& cfg);
    Result<void> restore_keyboard_defaults();

    // USB device enumeration and power‑management diagnostics
    Result<std::vector<USBDeviceInfo>> enumerate_usb_input_devices();
    Result<void> suggest_usb_power_settings(const USBDeviceInfo& dev);

    // Latency measurement – start/stop a low‑level hook that fills stats.
    Result<void> start_latency_measurement();
    Result<LatencyStats> stop_latency_measurement();
    // Called by the low-level hook callbacks installed in start_latency_measurement().
    void record_latency_event();

    // Optimization profiles (pre‑defined safe settings)
    enum class Profile { CompetitiveGaming, EmulatorGaming, StandardWindows };
    Result<void> apply_profile(Profile p);

private:
    // Helper for registry read/write with backup
    Result<DWORD> read_registry_dword(HKEY root, const std::wstring& subkey, const std::wstring& value);
    Result<void> write_registry_dword(HKEY root, const std::wstring& subkey, const std::wstring& value, DWORD data);
    Result<void> backup_registry_key(const std::wstring& description);

    // Hook handles for latency measurement (private implementation detail)
    HHOOK _hook = nullptr;
    HHOOK _kb_hook = nullptr;
    std::vector<LatencySample> _latency_samples;
    mutable std::mutex _sample_mtx;
    LARGE_INTEGER _last_timestamp{};
    LARGE_INTEGER _frequency{};
};
