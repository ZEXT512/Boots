#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "ProcessInfo.h"
#include "../common/Result.h"
#include "../common/Logger.h"
#include <nlohmann/json.hpp>

/**
 * ProcessManager – real‑time process scanner, classification, safe termination, and whitelist handling.
 *
 *   • refresh() – populates the internal process list using native Windows APIs.
 *   • get_processes() – returns the latest snapshot.
 *   • classify(const ProcessInfo&) – returns a Classification enum.
 *   • terminate(pid) – attempts safe termination respecting whitelists and critical lists.
 *   • whitelist management – add/remove/check.
 */
class ProcessManager {
public:
    enum class Classification {
        WindowsCritical,
        SecurityCritical,
        DriverRelated,
        UserApplication,
        BackgroundApplication,
        OptionalStartup,
        Unknown
    };

    // Initialise the manager (load whitelist from file)
    Result<void> initialize();
    Result<void> shutdown();

    // Refresh the internal snapshot (call periodically, e.g., every second)
    Result<void> refresh();

    // Accessors
    const std::vector<ProcessInfo>& get_processes() const { return _processes; }
    Classification classify(const ProcessInfo& info) const;

    // Safe termination – returns explanation string on failure or success message.
    Result<std::string> terminate_process(DWORD pid);

    // Whitelist handling
    Result<void> add_to_whitelist(const std::wstring& exe_name);
    Result<void> remove_from_whitelist(const std::wstring& exe_name);
    bool is_whitelisted(const std::wstring& exe_name) const;

private:
    std::vector<ProcessInfo> _processes;
    std::unordered_set<std::wstring> _whitelist;
    std::unordered_set<std::wstring> _critical_windows;
    std::unordered_set<std::wstring> _critical_security;
    std::unordered_set<std::wstring> _driver_related;
    std::wstring _whitelist_path;

    // Helpers
    std::wstring get_process_name(DWORD pid) const;
    SIZE_T get_memory_usage(HANDLE hProcess) const;
    double get_cpu_usage(DWORD pid) const; // placeholder – real implementation uses PDH
    void load_builtin_critical_lists();
    Result<void> load_whitelist();
    Result<void> save_whitelist() const;
};
