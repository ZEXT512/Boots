#include "InputLatency.h"
#include <winreg.h>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <SetupAPI.h>
#include <devguid.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <winusb.h>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <numeric>
#include <cmath>

using json = nlohmann::json;

namespace {
    // Low-level hooks deliver callbacks to the thread that installed them, so
    // the callbacks need a stable pointer to the active optimizer. A window
    // property is unreliable (the foreground window changes); an atomic is not.
    std::atomic<InputLatencyOptimizer*> g_active_optimizer{nullptr};
}

namespace {
    std::wstring program_data_path() {
        wchar_t* path = nullptr;
        size_t len = 0;
        _wdupenv_s(&path, &len, L"ProgramData");
        std::wstring result = path ? path : L"C:\\ProgramData";
        free(path);
        return result;
    }
    std::wstring backup_dir() {
        return program_data_path() + L"\\BOOTFPS\\registry_backups";
    }
    std::wstring timestamp_str() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t buf[64];
        swprintf(buf, 64, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::wstring(buf);
    }
}

Result<void> InputLatencyOptimizer::initialize() {
    // Ensure backup directory exists
    std::error_code ec;
    std::filesystem::create_directories(backup_dir(), ec);
    if (ec) {
        return Result<void>::Err("Failed to create backup directory: " + ec.message());
    }
    Logger::instance().log(Logger::Level::Info, L"InputLatencyOptimizer initialized.");
    return Result<void>::Ok();
}

Result<void> InputLatencyOptimizer::shutdown() {
    g_active_optimizer.store(nullptr);
    // If hooks are active, remove them
    if (_hook) {
        UnhookWindowsHookEx(_hook);
        _hook = nullptr;
    }
    if (_kb_hook) {
        UnhookWindowsHookEx(_kb_hook);
        _kb_hook = nullptr;
    }
    Logger::instance().log(Logger::Level::Info, L"InputLatencyOptimizer shutdown.");
    return Result<void>::Ok();
}

// ---------------- Mouse ----------------
Result<MouseConfig> InputLatencyOptimizer::read_mouse_config() {
    MouseConfig cfg{};
    // Pointer speed (0‑20, default 10)
    int speed = 0;
    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0)) {
        return Result<MouseConfig>::Err("Failed to retrieve mouse speed.");
    }
    cfg.pointer_speed = speed;
    // Enhance Pointer Precision (mouse acceleration) lives in the third slot
    // of the SPI_GETMOUSE array (0 = off, 1/2 = on). There is no
    // SPI_GETMOUSEACCELERATE constant in the Windows SDK.
    int mouseParams[3] = { 0, 0, 0 };
    if (!SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParams, 0)) {
        return Result<MouseConfig>::Err("Failed to retrieve mouse acceleration flag.");
    }
    cfg.enhance_pointer_precision = mouseParams[2] != 0;
    return Result<MouseConfig>::Ok(cfg);
}

Result<void> InputLatencyOptimizer::apply_mouse_profile(const MouseConfig& cfg) {
    // Backup current values first
    backup_registry_key(L"Mouse configuration backup before apply");
    // Apply pointer speed
    if (!SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)cfg.pointer_speed, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        return Result<void>::Err("Failed to set mouse speed.");
    }
    // Apply Enhance Pointer Precision through SPI_SETMOUSE (third array slot).
    int mouseParams[3] = { 0, 0, 0 };
    if (!SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParams, 0)) {
        return Result<void>::Err("Failed to read current mouse acceleration state.");
    }
    mouseParams[2] = cfg.enhance_pointer_precision ? 1 : 0;
    if (!SystemParametersInfoW(SPI_SETMOUSE, 0, mouseParams, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        return Result<void>::Err("Failed to set mouse acceleration.");
    }
    Logger::instance().log(Logger::Level::Info, L"Applied mouse profile: speed=" + std::to_wstring(cfg.pointer_speed) + L" accel=" + (cfg.enhance_pointer_precision ? L"1" : L"0"));
    return Result<void>::Ok();
}

Result<void> InputLatencyOptimizer::restore_mouse_defaults() {
    // Windows defaults: speed 10, acceleration enabled
    MouseConfig def{};
    def.pointer_speed = 10;
    def.enhance_pointer_precision = true;
    return apply_mouse_profile(def);
}

// ---------------- Keyboard ----------------
Result<KeyboardConfig> InputLatencyOptimizer::read_keyboard_config() {
    KeyboardConfig cfg{};
    // Repeat delay (0‑3) where actual delay = (cfg+1)*250ms
    int delay = 0;
    if (!SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0)) {
        return Result<KeyboardConfig>::Err("Failed to get keyboard repeat delay.");
    }
    cfg.repeat_delay = delay;
    // Repeat rate (0‑31) where rate = 2.5 + 0.5*cfg (Hz?)
    int rate = 0;
    if (!SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &rate, 0)) {
        return Result<KeyboardConfig>::Err("Failed to get keyboard repeat rate.");
    }
    cfg.repeat_rate = rate;
    return Result<KeyboardConfig>::Ok(cfg);
}

Result<void> InputLatencyOptimizer::apply_keyboard_profile(const KeyboardConfig& cfg) {
    backup_registry_key(L"Keyboard configuration backup before apply");
    if (!SystemParametersInfoW(SPI_SETKEYBOARDDELAY, cfg.repeat_delay, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        return Result<void>::Err("Failed to set keyboard repeat delay.");
    }
    if (!SystemParametersInfoW(SPI_SETKEYBOARDSPEED, cfg.repeat_rate, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        return Result<void>::Err("Failed to set keyboard repeat rate.");
    }
    Logger::instance().log(Logger::Level::Info, L"Applied keyboard profile: delay=" + std::to_wstring(cfg.repeat_delay) + L" rate=" + std::to_wstring(cfg.repeat_rate));
    return Result<void>::Ok();
}

Result<void> InputLatencyOptimizer::restore_keyboard_defaults() {
    // Windows defaults: delay 0 (250ms), rate 31 (fastest)
    KeyboardConfig def{};
    def.repeat_delay = 0;
    def.repeat_rate = 31;
    return apply_keyboard_profile(def);
}

// ---------------- USB Devices ----------------
Result<std::vector<USBDeviceInfo>> InputLatencyOptimizer::enumerate_usb_input_devices() {
    std::vector<USBDeviceInfo> out;
    // Enumerate every present HID interface. Each mouse/keyboard exposes at
    // least one HID collection, so this is the canonical way to find them.
    GUID hidGuid = GUID_DEVINTERFACE_HID;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        return Result<std::vector<USBDeviceInfo>>::Err("SetupDiGetClassDevs failed.");
    }

    for (DWORD index = 0; ; ++index) {
        SP_DEVICE_INTERFACE_DATA ifaceData{};
        ifaceData.cbSize = sizeof(ifaceData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, index, &ifaceData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;

        std::vector<BYTE> buffer(requiredSize);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail, requiredSize,
                                              nullptr, nullptr)) {
            continue;
        }

        USBDeviceInfo info{};
        info.device_path = detail->DevicePath;
        info.power_saving_enabled = false;

        // Read access is required by HidD_GetPreparsedData; some devices only
        // allow a zero-access handle, so fall back to that before giving up.
        HANDLE hDevice = CreateFileW(detail->DevicePath, GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
        if (hDevice == INVALID_HANDLE_VALUE) {
            hDevice = CreateFileW(detail->DevicePath, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        }

        if (hDevice != INVALID_HANDLE_VALUE) {
            wchar_t product[256] = {};
            if (HidD_GetProductString(hDevice, product, sizeof(product)) && product[0]) {
                info.description = product;
            }
            PHIDP_PREPARSED_DATA preparsed = nullptr;
            if (HidD_GetPreparsedData(hDevice, &preparsed) && preparsed) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == HID_USAGE_PAGE_GENERIC) {
                    info.is_mouse = caps.Usage == HID_USAGE_GENERIC_MOUSE;
                    info.is_keyboard = caps.Usage == HID_USAGE_GENERIC_KEYBOARD;
                }
                HidD_FreePreparsedData(preparsed);
            }
            CloseHandle(hDevice);
        }

        // Skip vendor-specific collections (RGB controllers, sensors, …) so the
        // table only lists actual input devices.
        if (!info.is_mouse && !info.is_keyboard) continue;

        if (info.description.empty()) {
            info.description = detail->DevicePath;
        }
        out.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return Result<std::vector<USBDeviceInfo>>::Ok(out);
}

Result<void> InputLatencyOptimizer::suggest_usb_power_settings(const USBDeviceInfo& dev) {
    // Simple recommendation based on heuristic – do NOT change any setting automatically.
    if (dev.is_mouse || dev.is_keyboard) {
        // Advise to keep selective suspend disabled if latency issues observed.
        Logger::instance().log(Logger::Level::Info, L"USB power recommendation for " + dev.description + L": ensure selective suspend is OFF for low latency.");
        return Result<void>::Ok();
    }
    return Result<void>::Err("Device not recognized as mouse/keyboard.");
}

// ---------------- Latency Measurement ----------------
static LRESULT CALLBACK LowLevelMouseHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        // Mouse hook: event timing is handled by record_latency_event().
        if (auto* optimizer = g_active_optimizer.load()) optimizer->record_latency_event();
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static LRESULT CALLBACK LowLevelKeyboardHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (auto* optimizer = g_active_optimizer.load()) optimizer->record_latency_event();
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void InputLatencyOptimizer::record_latency_event() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard<std::mutex> lock(_sample_mtx);
    if (_last_timestamp.QuadPart != 0 && _frequency.QuadPart != 0) {
        double interval = static_cast<double>(now.QuadPart - _last_timestamp.QuadPart) * 1000.0 / static_cast<double>(_frequency.QuadPart);
        _latency_samples.push_back({ interval });
    }
    _last_timestamp = now;
}

Result<void> InputLatencyOptimizer::start_latency_measurement() {
    // Reset data
    {
        std::lock_guard<std::mutex> lock(_sample_mtx);
        _latency_samples.clear();
        _last_timestamp.QuadPart = 0;
    }
    // Get high‑resolution timer frequency
    QueryPerformanceFrequency(&_frequency);
    // Publish the instance for the hook callbacks.
    g_active_optimizer.store(this);
    // Install low‑level hooks
    _hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseHook, nullptr, 0);
    if (!_hook) {
        return Result<void>::Err("Failed to install mouse hook.");
    }
    // Also install keyboard hook
    _kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardHook, nullptr, 0);
    if (!_kb_hook) {
        UnhookWindowsHookEx(_hook);
        _hook = nullptr;
        return Result<void>::Err("Failed to install keyboard hook.");
    }
    Logger::instance().log(Logger::Level::Info, L"Latency measurement started.");
    return Result<void>::Ok();
}

Result<LatencyStats> InputLatencyOptimizer::stop_latency_measurement() {
    if (_hook) {
        UnhookWindowsHookEx(_hook);
        _hook = nullptr;
    }
    if (_kb_hook) {
        UnhookWindowsHookEx(_kb_hook);
        _kb_hook = nullptr;
    }
    // Remove stored pointer
    g_active_optimizer.store(nullptr);
    if (_latency_samples.empty()) {
        return Result<LatencyStats>::Err("No latency samples collected.");
    }
    // Compute statistics
    LatencyStats stats{};
    {
        std::lock_guard<std::mutex> lock(_sample_mtx);
        stats.samples = _latency_samples;
    }
    std::vector<double> vals;
    vals.reserve(stats.samples.size());
    for (const auto& s : stats.samples) vals.push_back(s.interval_ms);
    stats.min_ms = *std::min_element(vals.begin(), vals.end());
    stats.max_ms = *std::max_element(vals.begin(), vals.end());
    double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
    stats.average_ms = sum / vals.size();
    double sq_sum = std::inner_product(vals.begin(), vals.end(), vals.begin(), 0.0);
    stats.stddev_ms = std::sqrt(sq_sum / vals.size() - stats.average_ms * stats.average_ms);
    Logger::instance().log(Logger::Level::Info, L"Latency measurement stopped – samples=" + std::to_wstring(_latency_samples.size()));
    return Result<LatencyStats>::Ok(stats);
}

// ---------------- Profiles ----------------
Result<void> InputLatencyOptimizer::apply_profile(Profile p) {
    switch (p) {
        case Profile::CompetitiveGaming: {
            // Disable pointer acceleration, set max pointer speed, tighten keyboard repeat rate.
            MouseConfig mouse{}; mouse.pointer_speed = 20; mouse.enhance_pointer_precision = false;
            KeyboardConfig kb{}; kb.repeat_delay = 0; kb.repeat_rate = 31;
            auto mr = apply_mouse_profile(mouse);
            if (mr.is_err()) return Result<void>::Err(mr.error());
            auto kr = apply_keyboard_profile(kb);
            if (kr.is_err()) return Result<void>::Err(kr.error());
            break;
        }
        case Profile::EmulatorGaming: {
            // Slightly lower pointer speed, keep acceleration, moderate keyboard repeat.
            MouseConfig mouse{}; mouse.pointer_speed = 12; mouse.enhance_pointer_precision = true;
            KeyboardConfig kb{}; kb.repeat_delay = 1; kb.repeat_rate = 20;
            auto mr = apply_mouse_profile(mouse);
            if (mr.is_err()) return Result<void>::Err(mr.error());
            auto kr = apply_keyboard_profile(kb);
            if (kr.is_err()) return Result<void>::Err(kr.error());
            break;
        }
        case Profile::StandardWindows: {
            // Restore defaults
            auto mr = restore_mouse_defaults();
            if (mr.is_err()) return Result<void>::Err(mr.error());
            auto kr = restore_keyboard_defaults();
            if (kr.is_err()) return Result<void>::Err(kr.error());
            break;
        }
        default:
            return Result<void>::Err("Unknown profile.");
    }
    Logger::instance().log(Logger::Level::Info, L"Applied input latency profile.");
    return Result<void>::Ok();
}

// ---------------- Registry Helpers ----------------
Result<DWORD> InputLatencyOptimizer::read_registry_dword(HKEY root, const std::wstring& subkey, const std::wstring& value) {
    HKEY hKey;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return Result<DWORD>::Err("Failed to open registry key for read.");
    }
    DWORD data = 0;
    DWORD size = sizeof(data);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(hKey, value.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(&data), &size);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        return Result<DWORD>::Err("Failed to query DWORD value.");
    }
    return Result<DWORD>::Ok(data);
}

Result<void> InputLatencyOptimizer::write_registry_dword(HKEY root, const std::wstring& subkey, const std::wstring& value, DWORD data) {
    HKEY hKey;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return Result<void>::Err("Failed to open/create registry key for write.");
    }
    LONG rc = RegSetValueExW(hKey, value.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("Failed to set DWORD registry value.");
    }
    return Result<void>::Ok();
}

Result<void> InputLatencyOptimizer::backup_registry_key(const std::wstring& description) {
    // Very simple backup: store description and timestamp in a JSON file.
    json j;
    j["description"] = std::string(description.begin(), description.end());
    j["timestamp"] = std::string(timestamp_str().begin(), timestamp_str().end());
    // For a real implementation we would enumerate and copy specific keys we intend to modify.
    std::wstring file = backup_dir() + L"\\" + timestamp_str() + L".json";
    std::ofstream out(file);
    if (!out) {
        return Result<void>::Err("Failed to write registry backup file.");
    }
    out << j.dump(4);
    Logger::instance().log(Logger::Level::Info, L"Created registry backup: " + file);
    return Result<void>::Ok();
}
