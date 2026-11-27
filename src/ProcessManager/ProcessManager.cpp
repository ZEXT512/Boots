#include "ProcessManager.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <mutex>
#include <pdh.h>
#include <pdhmsg.h>
#include <windows.h>
#include <psapi.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
    std::wstring program_data_path() {
        wchar_t* path = nullptr;
        size_t len = 0;
        _wdupenv_s(&path, &len, L"ProgramData");
        std::wstring result = path ? path : L"C:\\ProgramData";
        free(path);
        return result;
    }
}

Result<void> ProcessManager::initialize() {
    // Determine whitelist file location
    _whitelist_path = program_data_path() + L"\\BOOTFPS\\whitelist.json";
    load_builtin_critical_lists();
    return load_whitelist();
}

Result<void> ProcessManager::shutdown() {
    // Save whitelist on shutdown
    return save_whitelist();
}

void ProcessManager::load_builtin_critical_lists() {
    // Very basic hard‑coded lists – can be expanded later
    const std::vector<std::wstring> windows = { L"csrss.exe", L"wininit.exe", L"services.exe", L"lsass.exe", L"smss.exe", L"svchost.exe", L"winlogon.exe", L"explorer.exe" };
    const std::vector<std::wstring> security = { L"MsMpEng.exe", L"SecurityHealthService.exe", L"wscsvc.exe", L"windefend.exe" };
    const std::vector<std::wstring> drivers = { L"ntoskrnl.exe", L"hal.dll" };
    _critical_windows.insert(windows.begin(), windows.end());
    _critical_security.insert(security.begin(), security.end());
    _driver_related.insert(drivers.begin(), drivers.end());
}

Result<void> ProcessManager::load_whitelist() {
    std::ifstream in(_whitelist_path);
    if (!in) {
        // No whitelist yet – start empty
        Logger::instance().log(Logger::Level::Info, L"Whitelist file not found, starting with empty list.");
        return Result<void>::Ok();
    }
    json j;
    try {
        in >> j;
        for (const auto& item : j) {
            if (item.is_string()) {
                std::wstring exe = std::wstring(item.get<std::wstring>());
                _whitelist.insert(exe);
            }
        }
    } catch (const std::exception& e) {
        return Result<void>::Err(std::string("Failed to parse whitelist: ") + e.what());
    }
    Logger::instance().log(Logger::Level::Info, L"Whitelist loaded, entries: " + std::to_wstring(_whitelist.size()));
    return Result<void>::Ok();
}

Result<void> ProcessManager::save_whitelist() const {
    std::filesystem::path dir = std::filesystem::path(_whitelist_path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Result<void>::Err("Failed to create whitelist directory: " + ec.message());
    }
    std::ofstream out(_whitelist_path);
    if (!out) {
        return Result<void>::Err("Unable to open whitelist file for writing");
    }
    json j = json::array();
    for (const auto& exe : _whitelist) {
        j.push_back(std::string(exe.begin(), exe.end())); // store as UTF‑8
    }
    out << j.dump(4);
    Logger::instance().log(Logger::Level::Info, L"Whitelist saved.");
    return Result<void>::Ok();
}

std::wstring ProcessManager::get_process_name(DWORD pid) const {
    wchar_t name[MAX_PATH] = L"";
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return L"";
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(hSnap, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                wcsncpy_s(name, entry.szExeFile, _TRUNCATE);
                break;
            }
        } while (Process32NextW(hSnap, &entry));
    }
    CloseHandle(hSnap);
    return std::wstring(name);
}

SIZE_T ProcessManager::get_memory_usage(HANDLE hProcess) const {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(hProcess, (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) {
        return counters.WorkingSetSize;
    }
    return 0;
}

double ProcessManager::get_cpu_usage(DWORD pid) const {
    // Compute CPU usage based on elapsed process time between snapshots.
    // This implementation relies on data stored during refresh(); if not present, returns 0.
    static std::mutex mtx; // protect shared static maps
    std::lock_guard<std::mutex> lock(mtx);
    // Maps are members of the class – we keep them as mutable for const method.
    // For simplicity we store them in mutable members (declared below).
    return 0.0; // placeholder – actual value filled in refresh().
}

Result<void> ProcessManager::refresh() {
    _processes.clear();
    // Store previous CPU data for delta calculation
    static std::unordered_map<DWORD, ULONGLONG> prevProcTime; // 100‑ns units
    static std::unordered_map<DWORD, std::chrono::steady_clock::time_point> prevTimestamp;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return Result<void>::Err("CreateToolhelp32Snapshot failed");
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(hSnap, &entry)) {
        CloseHandle(hSnap);
        return Result<void>::Err("Process32FirstW failed");
    }
    do {
        ProcessInfo info{};
        info.pid = entry.th32ProcessID;
        info.name = entry.szExeFile;
        // Open process for query
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, info.pid);
        if (hProc) {
            // Memory usage
            info.memory_bytes = get_memory_usage(hProc);
            // CPU usage – compute delta from previous snapshot
            FILETIME ftCreation, ftExit, ftKernel, ftUser;
            if (GetProcessTimes(hProc, &ftCreation, &ftExit, &ftKernel, &ftUser)) {
                ULONGLONG totalTime = (static_cast<ULONGLONG>(ftKernel.dwLowDateTime) | (static_cast<ULONGLONG>(ftKernel.dwHighDateTime) << 32))
                                    + (static_cast<ULONGLONG>(ftUser.dwLowDateTime) | (static_cast<ULONGLONG>(ftUser.dwHighDateTime) << 32));
                auto now = std::chrono::steady_clock::now();
                if (prevProcTime.find(info.pid) != prevProcTime.end() && prevTimestamp.find(info.pid) != prevTimestamp.end()) {
                    ULONGLONG prevTime = prevProcTime[info.pid];
                    auto prevTp = prevTimestamp[info.pid];
                    double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(now - prevTp).count();
                    double delta = static_cast<double>(totalTime - prevTime) / 1e7; // convert 100‑ns to seconds
                    // Number of logical CPUs
                    SYSTEM_INFO sysInfo{};
                    GetSystemInfo(&sysInfo);
                    double cpuPercent = (delta / elapsedSec) * 100.0 / static_cast<double>(sysInfo.dwNumberOfProcessors);
                    info.cpu_percent = std::max(0.0, std::min(cpuPercent, 100.0));
                } else {
                    info.cpu_percent = 0.0; // first measurement
                }
                // Store for next refresh
                prevProcTime[info.pid] = totalTime;
                prevTimestamp[info.pid] = now;
            } else {
                info.cpu_percent = 0.0;
            }
            CloseHandle(hProc);
        } else {
            info.memory_bytes = 0;
            info.cpu_percent = 0.0;
        }
        // Heuristics for background / high‑cpu / user app
        info.is_high_cpu = info.cpu_percent > 20.0; // threshold adjustable
        // Background heuristic: no visible window and low CPU
        info.is_background = (!info.is_high_cpu && info.memory_bytes < 100 * 1024 * 1024);
        // User app heuristic – not in critical lists and has a .exe name other than system services
        info.is_user_app = _critical_windows.find(info.name) == _critical_windows.end();
        _processes.push_back(std::move(info));
    } while (Process32NextW(hSnap, &entry));
    CloseHandle(hSnap);
    Logger::instance().log(Logger::Level::Debug, L"Process snapshot refreshed, count=" + std::to_wstring(_processes.size()));
    return Result<void>::Ok();
}

ProcessManager::Classification ProcessManager::classify(const ProcessInfo& info) const {
    std::wstring lname = info.name;
    std::transform(lname.begin(), lname.end(), lname.begin(), ::towlower);
    if (_critical_windows.find(lname) != _critical_windows.end()) return Classification::WindowsCritical;
    if (_critical_security.find(lname) != _critical_security.end()) return Classification::SecurityCritical;
    if (_driver_related.find(lname) != _driver_related.end()) return Classification::DriverRelated;
    if (info.is_background) return Classification::BackgroundApplication;
    if (info.is_user_app) return Classification::UserApplication;
    // Optional startup heuristic – processes with low CPU and started at boot (not implemented). For now treat as Unknown.
    return Classification::Unknown;
}

Result<std::string> ProcessManager::terminate_process(DWORD pid) {
    // Find process info
    auto it = std::find_if(_processes.begin(), _processes.end(), [pid](const ProcessInfo& p){ return p.pid == pid; });
    if (it == _processes.end()) {
        return Result<std::string>::Err("Process not found in current snapshot");
    }
    const ProcessInfo& info = *it;
    // Whitelist check
    if (is_whitelisted(info.name)) {
        return Result<std::string>::Err("Process is on user whitelist and cannot be terminated.");
    }
    // Critical checks
    Classification cls = classify(info);
    if (cls == Classification::WindowsCritical) {
        return Result<std::string>::Err("Process is Windows‑critical; terminating it may destabilise the system.");
    }
    if (cls == Classification::SecurityCritical) {
        return Result<std::string>::Err("Process is security‑critical (antivirus, defender); terminating it is unsafe.");
    }
    if (cls == Classification::DriverRelated) {
        return Result<std::string>::Err("Process is driver‑related; terminating it may cause hardware issues.");
    }
    // Safe to terminate – request confirmation handled by UI, here we just attempt.
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) {
        return Result<std::string>::Err("Failed to obtain handle for termination (insufficient rights?).");
    }
    BOOL result = TerminateProcess(hProc, 0);
    DWORD err = GetLastError();
    CloseHandle(hProc);
    if (!result) {
        return Result<std::string>::Err("TerminateProcess failed, error code: " + std::to_string(err));
    }
    std::string msg = "Process " + std::string(info.name.begin(), info.name.end()) + " (PID " + std::to_string(pid) + ") terminated successfully.";
    Logger::instance().log(Logger::Level::Info, L"Terminated process: " + info.name + L" PID=" + std::to_wstring(pid));
    return Result<std::string>::Ok(msg);
}

Result<void> ProcessManager::add_to_whitelist(const std::wstring& exe_name) {
    _whitelist.insert(exe_name);
    return save_whitelist();
}

Result<void> ProcessManager::remove_from_whitelist(const std::wstring& exe_name) {
    _whitelist.erase(exe_name);
    return save_whitelist();
}

bool ProcessManager::is_whitelisted(const std::wstring& exe_name) const {
    return _whitelist.find(exe_name) != _whitelist.end();
}
