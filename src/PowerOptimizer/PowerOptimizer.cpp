#include "PowerOptimizer.h"
#include <powrprof.h>
#include <shlwapi.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <cstdlib>
#include <nlohmann/json.hpp>

#pragma comment(lib, "PowrProf.lib")

using json = nlohmann::json;

namespace {
    std::wstring program_data_path() {
        wchar_t* p = nullptr;
        size_t len = 0;
        _wdupenv_s(&p, &len, L"ProgramData");
        std::wstring res = p ? p : L"C:\\ProgramData";
        free(p);
        return res;
    }
}

// Definitions for the class helpers declared in PowerOptimizer.h.
std::wstring PowerOptimizer::guid_to_wstring(const GUID& guid) {
    wchar_t buf[64] = {};
    StringFromGUID2(guid, buf, 64);
    return std::wstring(buf);
}

GUID PowerOptimizer::wstring_to_guid(const std::wstring& str) {
    GUID guid{};
    CLSIDFromString(str.c_str(), &guid);
    return guid;
}

Result<void> PowerOptimizer::initialize() {
    // Create backup directory
    _backup_dir = program_data_path() + L"\\BOOTFPS\\power_backups";
    std::error_code ec;
    std::filesystem::create_directories(_backup_dir, ec);
    if (ec) {
        return Result<void>::Err("Failed to create backup directory: " + ec.message());
    }
    // Save original active scheme GUID
    auto active_res = get_active_scheme();
    if (active_res.is_err()) return Result<void>::Err(active_res.error());
    _original_guid = active_res.unwrap();
    Logger::instance().log(Logger::Level::Info, L"PowerOptimizer initialized, original scheme saved.");
    return Result<void>::Ok();
}

Result<void> PowerOptimizer::shutdown() {
    Logger::instance().log(Logger::Level::Info, L"PowerOptimizer shutdown.");
    return Result<void>::Ok();
}

// ---------- Low‑level wrappers ----------
Result<GUID> PowerOptimizer::get_active_scheme() {
    // PowerGetActiveScheme allocates the buffer with LocalAlloc; do not
    // pre-allocate it with malloc.
    GUID* pGuid = nullptr;
    DWORD rc = PowerGetActiveScheme(nullptr, &pGuid);
    if (rc != ERROR_SUCCESS) {
        if (pGuid) LocalFree(pGuid);
        return Result<GUID>::Err("PowerGetActiveScheme failed, code=" + std::to_string(rc));
    }
    GUID guid = *pGuid;
    LocalFree(pGuid);
    return Result<GUID>::Ok(guid);
}

Result<void> PowerOptimizer::set_active_scheme(const GUID& guid) {
    DWORD rc = PowerSetActiveScheme(nullptr, &guid);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("PowerSetActiveScheme failed, code=" + std::to_string(rc));
    }
    Logger::instance().log(Logger::Level::Info, L"Power scheme set to " + guid_to_wstring(guid));
    return Result<void>::Ok();
}

Result<std::vector<PowerPlan>> PowerOptimizer::enumerate_schemes() {
    std::vector<PowerPlan> out;
    DWORD index = 0;
    while (true) {
        GUID guid{};
        DWORD guidSize = sizeof(GUID);
        // The SDK declares the buffer as UCHAR*; it receives a GUID for ACCESS_SCHEME.
        DWORD rc = PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, reinterpret_cast<UCHAR*>(&guid), &guidSize);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) {
            return Result<std::vector<PowerPlan>>::Err("PowerEnumerate failed at index " + std::to_string(index) + ", code=" + std::to_string(rc));
        }
        PowerPlan plan{};
        plan.guid = guid;
        // Retrieve name
        auto name_res = get_scheme_name(guid);
        if (name_res.is_ok()) plan.name = name_res.unwrap();
        else plan.name = L"(unknown)";
        out.push_back(plan);
        ++index;
    }
    return Result<std::vector<PowerPlan>>::Ok(out);
}

Result<std::wstring> PowerOptimizer::get_scheme_name(const GUID& guid) {
    // PowerReadFriendlyName requires a valid BufferSize pointer. Query the
    // required size first, then read the string.
    DWORD size = 0;
    DWORD rc = PowerReadFriendlyName(nullptr, &guid, nullptr, nullptr, nullptr, &size);
    if (rc == ERROR_SUCCESS && size > 0) {
        std::vector<UCHAR> buffer(size);
        rc = PowerReadFriendlyName(nullptr, &guid, nullptr, nullptr, buffer.data(), &size);
        if (rc == ERROR_SUCCESS) {
            std::wstring name(reinterpret_cast<const wchar_t*>(buffer.data()));
            if (!name.empty()) {
                // Built-in schemes return indirect strings such as
                // "@%SystemRoot%\system32\powrprof.dll,-1"; resolve them.
                if (name.front() == L'@') {
                    wchar_t resolved[512] = {};
                    if (SUCCEEDED(SHLoadIndirectString(name.c_str(), resolved, 512, nullptr))) {
                        name = resolved;
                    }
                }
                return Result<std::wstring>::Ok(name);
            }
        }
    }

    // Fallback: read the friendly name straight from the registry.
    HKEY hKey = nullptr;
    wchar_t subkey[64] = {};
    StringFromGUID2(guid, subkey, 64);
    std::wstring path = L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes\\" + std::wstring(subkey);
    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS) {
        return Result<std::wstring>::Err("RegOpenKeyEx failed for scheme name, code=" + std::to_string(rc));
    }
    wchar_t name[128] = {};
    DWORD nameSize = sizeof(name);
    rc = RegQueryValueExW(hKey, L"FriendlyName", nullptr, nullptr, (LPBYTE)name, &nameSize);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        return Result<std::wstring>::Err("RegQueryValueEx for FriendlyName failed, code=" + std::to_string(rc));
    }
    return Result<std::wstring>::Ok(std::wstring(name));
}

Result<void> PowerOptimizer::set_scheme_name(const GUID& guid, const std::wstring& name) {
    wchar_t subkey[256];
    StringFromGUID2(guid, subkey, 256);
    std::wstring path = L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes\\" + std::wstring(subkey);
    HKEY hKey;
    DWORD rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("RegCreateKeyEx failed for setting name, code=" + std::to_string(rc));
    }
    rc = RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ, (const BYTE*)name.c_str(), static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("RegSetValueEx for FriendlyName failed, code=" + std::to_string(rc));
    }
    return Result<void>::Ok();
}

// ---------- Public API ----------
Result<PowerPlan> PowerOptimizer::get_current_plan() const {
    auto guid_res = get_active_scheme();
    if (guid_res.is_err()) return Result<PowerPlan>::Err(guid_res.error());
    GUID guid = guid_res.unwrap();
    auto name_res = get_scheme_name(guid);
    PowerPlan plan{};
    plan.guid = guid;
    plan.name = name_res.is_ok() ? name_res.unwrap() : L"(unknown)";
    plan.is_active = true;
    return Result<PowerPlan>::Ok(plan);
}

Result<std::vector<PowerPlan>> PowerOptimizer::list_available_plans() const {
    return enumerate_schemes();
}

Result<void> PowerOptimizer::set_plan(const PowerPlan& plan) {
    return set_active_scheme(plan.guid);
}

Result<GUID> PowerOptimizer::duplicate_scheme(const GUID& source_guid, const std::wstring& new_name) {
    // Create a copy of the source scheme using PowerDuplicateScheme (available on Windows 8+).
    // The API allocates the new GUID with LocalFree-compatible memory.
    GUID* pNewGuid = nullptr;
    DWORD rc = PowerDuplicateScheme(nullptr, &source_guid, &pNewGuid);
    if (rc != ERROR_SUCCESS || !pNewGuid) {
        if (pNewGuid) LocalFree(pNewGuid);
        return Result<GUID>::Err("PowerDuplicateScheme failed, code=" + std::to_string(rc));
    }
    GUID new_guid = *pNewGuid;
    LocalFree(pNewGuid);
    // Assign the friendly name
    auto name_res = set_scheme_name(new_guid, new_name);
    if (name_res.is_err()) {
        // Cleanup the newly created scheme to avoid littering the system
        PowerDeleteScheme(nullptr, &new_guid);
        return Result<GUID>::Err("Failed to set name for duplicated scheme: " + name_res.error());
    }
    return Result<GUID>::Ok(new_guid);
}

Result<void> PowerOptimizer::apply_profile(PowerProfile profile) {
    // Determine base scheme to copy – use Balanced as safe starting point.
    GUID base_guid = GUID{};
    // Find the built‑in Balanced scheme GUID (predefined constant)
    const GUID GUID_BALANCED = {0x381b4222,0xf694,0x41f0,{0x96,0x85,0xff,0x5b,0xb2,0x60,0xdf,0x2e}}; // POWER_SCHEME_BALANCED
    base_guid = GUID_BALANCED;
    std::wstring profile_name;
    switch (profile) {
        case PowerProfile::Balanced: profile_name = L"BOOTFPS Balanced"; break;
        case PowerProfile::Performance: profile_name = L"BOOTFPS Performance"; break;
        case PowerProfile::Emulator: profile_name = L"BOOTFPS Emulator"; break;
        case PowerProfile::Streaming: profile_name = L"BOOTFPS Streaming"; break;
        default: profile_name = L"BOOTFPS Custom"; break;
    }
    // Remove a scheme created by a previous run so repeated profile applies do
    // not leave a pile of "BOOTFPS ..." plans behind.
    auto existing = enumerate_schemes();
    if (existing.is_ok()) {
        for (const auto& plan : existing.unwrap()) {
            if (plan.name == profile_name) {
                GUID stale = plan.guid;
                PowerDeleteScheme(nullptr, &stale);
            }
        }
    }
    // Duplicate base scheme
    auto dup_res = duplicate_scheme(base_guid, profile_name);
    if (dup_res.is_err()) return Result<void>::Err(dup_res.error());
    GUID new_scheme = dup_res.unwrap();

    // Adjust settings per profile – we modify only AC Power settings (subgroup: PROCESSOR)
    // Subgroup GUID for Processor Settings
    const GUID GUID_PROCESSOR_SETTINGS = {0x54533251,0x82be,0x4824,{0x96,0xc1,0x47,0xb6,0x0b,0x74,0x0d,0x00}};
    const GUID GUID_PROCESSOR_POWER_THROTTLE = {0x893dee8e,0x2bef,0x41e0,{0x89,0xc6,0xb5,0x68,0xa8,0xe5,0x83,0x0b}};
    const GUID GUID_PROCESSOR_PERF_BOOST_POLICY = {0xbe337238,0x0d82,0x4146,{0xa9,0xd5,0x75,0xe1,0x58,0x20,0xa5,0x37}};
    // Helper to set a value (AC) using PowerWriteACValueIndex
    auto set_ac_value = [&](const GUID& sub_guid, const GUID& setting_guid, DWORD value) -> Result<void> {
        DWORD rc = PowerWriteACValueIndex(nullptr, &new_scheme, &sub_guid, &setting_guid, value);
        if (rc != ERROR_SUCCESS) {
            return Result<void>::Err("PowerWriteACValueIndex failed, rc=" + std::to_string(rc));
        }
        return Result<void>::Ok();
    };
    // Apply profile‑specific tweaks (examples, safe values)
    switch (profile) {
        case PowerProfile::Balanced:
            // Use system defaults – no changes needed after duplication.
            break;
        case PowerProfile::Performance:
            // Set Minimum processor state to 100% (no throttling), Maximum to 100%.
            set_ac_value(GUID_PROCESSOR_SETTINGS, GUID_PROCESSOR_POWER_THROTTLE, 100);
            // Boost policy: 0 = disabled, 1 = Aggressive. Use aggressive for performance.
            set_ac_value(GUID_PROCESSOR_SETTINGS, GUID_PROCESSOR_PERF_BOOST_POLICY, 1);
            break;
        case PowerProfile::Emulator:
            // Slightly lower min to avoid heat spikes but keep high max.
            set_ac_value(GUID_PROCESSOR_SETTINGS, GUID_PROCESSOR_POWER_THROTTLE, 80); // Min 80%
            set_ac_value(GUID_PROCESSOR_SETTINGS, GUID_PROCESSOR_PERF_BOOST_POLICY, 1);
            break;
        case PowerProfile::Streaming:
            // Keep max high but allow min to drop a bit for background encoders.
            set_ac_value(GUID_PROCESSOR_SETTINGS, GUID_PROCESSOR_POWER_THROTTLE, 70);
            set_ac_value(GUID_PROCESSOR_SETTINGS, GUID_PROCESSOR_PERF_BOOST_POLICY, 0); // less aggressive boost to keep temps lower.
            break;
        default:
            break;
    }
    // Commit changes
    DWORD rc = PowerSetActiveScheme(nullptr, &new_scheme);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("Failed to activate duplicated profile, rc=" + std::to_string(rc));
    }
    Logger::instance().log(Logger::Level::Info, L"Applied power profile " + profile_name);
    return Result<void>::Ok();
}

std::vector<std::string> PowerOptimizer::diagnose() const {
    std::vector<std::string> warnings;
    // 1. CPU frequency limitation – check if current max processor state < 100%
    GUID current_guid;
    auto cur_res = get_active_scheme();
    if (cur_res.is_ok()) current_guid = cur_res.unwrap();
    else return {"Failed to retrieve active power scheme for diagnostics."};
    // Retrieve Max Processor State (AC) – subGuid PROCESSOR_SETTINGS, setting GUID_PROCESSOR_THROTTLE_MAX (predefined)
    const GUID GUID_PROCESSOR_SETTINGS = {0x54533251,0x82be,0x4824,{0x96,0xc1,0x47,0xb6,0x0b,0x74,0x0d,0x00}};
    const GUID GUID_MAX_PROC_STATE = {0xbc5038f7,0x23e0,0x4960,{0x96,0xda,0x33,0xab,0xaf,0x59,0x35,0x50}}; // PROCESSOR_MAXIMUM
    DWORD acValue = 0; DWORD size = sizeof(DWORD);
    DWORD rc = PowerReadACValueIndex(nullptr, &current_guid, &GUID_PROCESSOR_SETTINGS, &GUID_MAX_PROC_STATE, &acValue);
    if (rc == ERROR_SUCCESS) {
        if (acValue < 100) warnings.push_back("CPU max processor state limited to " + std::to_string(acValue) + "% – may cap frequency.");
    } else {
        warnings.push_back("Unable to read CPU max processor state (rc=" + std::to_string(rc) + ").");
    }
    // 2. Power management restrictions – check if "Allow hybrid sleep" disabled, etc. (simple example)
    const GUID GUID_SLEEP_SUBGROUP = {0x238c9fa8,0x0aad,0x41ed,{0x83,0xf4,0x97,0xbe,0x24,0x2c,0x3f,0x85}};
    const GUID GUID_ALLOW_HYBRID_SLEEP = {0x94ac6d29,0x73ce,0x41a6,{0x80,0x77,0xbb,0xee,0x50,0x6f,0x4b,0x78}};
    DWORD hybrid = 0; size = sizeof(DWORD);
    rc = PowerReadACValueIndex(nullptr, &current_guid, &GUID_SLEEP_SUBGROUP, &GUID_ALLOW_HYBRID_SLEEP, &hybrid);
    if (rc == ERROR_SUCCESS && hybrid == 0) {
        warnings.push_back("Hybrid sleep disabled – may affect wake‑up latency.");
    }
    // 3. Potential power‑saving conflicts – e.g., USB selective suspend.
    const GUID GUID_USB_SUBGROUP = {0x2c5e0fe8,0x8786,0x49fb,{0x85,0x48,0x2a,0xc5,0xb5,0xed,0x45,0xe5}};
    const GUID GUID_USB_SELECTIVE_SUSPEND = {0x5d76a2ca,0xee48,0x4a10,{0xa1,0x5d,0x03,0x38,0x59,0x22,0x41,0x61}};
    DWORD usbSuspend = 0; size = sizeof(DWORD);
    rc = PowerReadACValueIndex(nullptr, &current_guid, &GUID_USB_SUBGROUP, &GUID_USB_SELECTIVE_SUSPEND, &usbSuspend);
    if (rc == ERROR_SUCCESS && usbSuspend == 1) {
        warnings.push_back("USB selective suspend enabled – may cause input latency spikes.");
    }
    // Add more checks as needed.
    if (warnings.empty()) warnings.push_back("No obvious power‑related issues detected.");
    return warnings;
}

Result<double> PowerOptimizer::benchmark() {
    // Simple CPU‑bound loop for ~1 second, measure elapsed time.
    const uint64_t iterations = 500'000'000ULL; // large enough to take noticeable time.
    auto start = std::chrono::high_resolution_clock::now();
    volatile uint64_t sum = 0; // volatile to avoid optimization away
    for (uint64_t i = 0; i < iterations; ++i) {
        sum += i;
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    Logger::instance().log(Logger::Level::Info, L"Benchmark completed in " + std::to_wstring(elapsed.count()) + L" seconds.");
    return Result<double>::Ok(elapsed.count());
}

Result<void> PowerOptimizer::restore_original() {
    // Restore the scheme saved at initialise()
    return set_active_scheme(_original_guid);
}
