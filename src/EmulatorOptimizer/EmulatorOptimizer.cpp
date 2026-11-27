#include "EmulatorOptimizer.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <windows.h>
#include <tlhelp32.h>
#include <processthreadsapi.h>
#include <psapi.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <algorithm>
#include <unordered_map>

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
    std::wstring timestamp() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t buf[64];
        swprintf(buf, 64, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::wstring(buf);
    }
    // Helper to check if a file exists
    bool file_exists(const std::wstring& path) {
        return std::filesystem::exists(path);
    }
    // Convert wstring to utf8 string
    std::string w2utf8(const std::wstring& ws) {
        int size = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
        std::string out(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), size, nullptr, nullptr);
        return out;
    }
    // Convert utf8 string to wstring
    std::wstring utf8_to_w(const std::string& s) {
        int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring out(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), size);
        return out;
    }
}

Result<void> EmulatorOptimizer::initialize() {
    _backup_root = program_data_path() + L"\\BOOTFPS\\emulator_backups";
    std::error_code ec;
    std::filesystem::create_directories(_backup_root, ec);
    if (ec) {
        return Result<void>::Err("Failed to create backup directory: " + ec.message());
    }
    Logger::instance().log(Logger::Level::Info, L"EmulatorOptimizer initialized, backup root = " + _backup_root);
    return Result<void>::Ok();
}

Result<void> EmulatorOptimizer::shutdown() {
    Logger::instance().log(Logger::Level::Info, L"EmulatorOptimizer shutdown.");
    return Result<void>::Ok();
}

// ---------------------------------------------------------------------
// Detection helpers
// ---------------------------------------------------------------------
Result<std::vector<EmulatorInstance>> EmulatorOptimizer::detect_installed() {
    std::vector<EmulatorInstance> out;
    // MSI App Player default path (64‑bit installations)
    const std::wstring msi_path = L"C:\\Program Files\\MSI APP Player";
    if (file_exists(msi_path + L"\\MsiApp.exe")) {
        auto res = probe_msi(msi_path);
        if (res.is_ok()) out.push_back(res.unwrap());
    }
    // BlueStacks default paths – we look for both 64‑bit and 32‑bit locations
    const std::wstring bs_path64 = L"C:\\Program Files\\BlueStacks_nxt"; // modern BlueStacks 5 installs here
    const std::wstring bs_path32 = L"C:\\Program Files (x86)\\BlueStacks_nxt";
    if (file_exists(bs_path64 + L"\\HD-Player.exe")) {
        auto res = probe_bluestacks(bs_path64);
        if (res.is_ok()) out.push_back(res.unwrap());
    }
    if (file_exists(bs_path32 + L"\\HD-Player.exe")) {
        auto res = probe_bluestacks(bs_path32);
        if (res.is_ok()) out.push_back(res.unwrap());
    }
    Logger::instance().log(Logger::Level::Info, L"Detected " + std::to_wstring(out.size()) + L" installed emulator(s).");
    return Result<std::vector<EmulatorInstance>>::Ok(out);
}

Result<std::vector<EmulatorInstance>> EmulatorOptimizer::detect_running(const std::vector<EmulatorInstance>& installed) {
    // Enumerate all processes once
    std::unordered_map<std::wstring, DWORD> proc_map; // exe name -> pid (first occurrence)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return Result<std::vector<EmulatorInstance>>::Err("Failed to create process snapshot.");
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring exe = pe.szExeFile;
            std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
            if (proc_map.find(exe) == proc_map.end()) {
                proc_map[exe] = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::vector<EmulatorInstance> result = installed;
    for (auto& inst : result) {
        std::wstring exe_name;
        if (inst.name == L"MSI App Player") exe_name = L"msiapp.exe"; // actual executable name may vary; use lower case
        else if (inst.name == L"BlueStacks") exe_name = L"hd-player.exe";
        else continue;
        auto it = proc_map.find(exe_name);
        if (it != proc_map.end()) {
            inst.is_running = true;
            inst.pid = it->second;
        } else {
            inst.is_running = false;
            inst.pid = 0;
        }
    }
    Logger::instance().log(Logger::Level::Info, L"Running detection completed.");
    return Result<std::vector<EmulatorInstance>>::Ok(result);
}

// ---------------------------------------------------------------------
// Probing specific emulators for version & install info
// ---------------------------------------------------------------------
Result<EmulatorInstance> EmulatorOptimizer::probe_msi(const std::wstring& base_dir) {
    EmulatorInstance ei{};
    ei.name = L"MSI App Player";
    ei.install_dir = base_dir;
    // Version – read from the exe file version resource (simplified: use file name if exists)
    const std::wstring exe_path = base_dir + L"\\MsiApp.exe";
    if (!file_exists(exe_path)) {
        return Result<EmulatorInstance>::Err("MSI executable not found.");
    }
    // Use GetFileVersionInfo for real version – omitted for brevity, set placeholder
    ei.version = L"unknown";
    // Default configuration values – will be overwritten by read_configuration()
    ei.configured_cpu_cores = 0;
    ei.configured_ram_mb = 0;
    ei.rendering_mode = L"";
    ei.graphics_backend = L"";
    ei.is_running = false;
    ei.pid = 0;
    return Result<EmulatorInstance>::Ok(ei);
}

Result<EmulatorInstance> EmulatorOptimizer::probe_bluestacks(const std::wstring& base_dir) {
    EmulatorInstance ei{};
    ei.name = L"BlueStacks";
    ei.install_dir = base_dir;
    const std::wstring exe_path = base_dir + L"\\HD-Player.exe";
    if (!file_exists(exe_path)) {
        return Result<EmulatorInstance>::Err("BlueStacks executable not found.");
    }
    ei.version = L"unknown"; // Placeholder – could parse from .exe version resource.
    ei.configured_cpu_cores = 0;
    ei.configured_ram_mb = 0;
    ei.rendering_mode = L"";
    ei.graphics_backend = L"";
    ei.is_running = false;
    ei.pid = 0;
    return Result<EmulatorInstance>::Ok(ei);
}

// ---------------------------------------------------------------------
// Configuration file handling (simple INI style key=value)
// ---------------------------------------------------------------------
Result<void> EmulatorOptimizer::read_configuration(EmulatorInstance& instance) {
    // Most emulators store config under install_dir\config\bluestacks.conf or similar.
    std::wstring cfg_path;
    if (instance.name == L"BlueStacks") {
        cfg_path = instance.install_dir + L"\\bluestacks.conf"; // typical name
    } else if (instance.name == L"MSI App Player") {
        cfg_path = instance.install_dir + L"\\config.ini"; // generic placeholder
    } else {
        return Result<void>::Err("Unsupported emulator for configuration read.");
    }
    if (!file_exists(cfg_path)) {
        return Result<void>::Err("Configuration file not found: " + std::string(cfg_path.begin(), cfg_path.end()));
    }
    // Read file as UTF‑8 text
    std::ifstream in(cfg_path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string content = buffer.str();
    auto parse_res = parse_ini(utf8_to_w(content));
    if (parse_res.is_err()) return Result<void>::Err(parse_res.error());
    const auto& map = parse_res.unwrap();
    // Helpers to fetch a numeric value safely
    auto get_int = [&](const std::wstring& key, int& out) {
        auto it = map.find(key);
        if (it != map.end()) {
            try {
                out = std::stoi(it->second);
                return true;
            } catch (...) { }
        }
        return false;
    };
    // Typical keys (may differ per emulator) – we try a few common ones.
    get_int(L"cpu_core_count", instance.configured_cpu_cores);
    get_int(L"memory_size_mb", instance.configured_ram_mb);
    auto it = map.find(L"renderer");
    if (it != map.end()) instance.rendering_mode = it->second;
    it = map.find(L"graphics_backend");
    if (it != map.end()) instance.graphics_backend = it->second;
    // Save the path for later writes
    // (not stored in struct; we compute again on write based on name)
    Logger::instance().log(Logger::Level::Info, L"Read configuration for " + instance.name + L" from " + cfg_path);
    return Result<void>::Ok();
}

Result<void> EmulatorOptimizer::write_configuration(const EmulatorInstance& instance) {
    // Determine config path same way as read_configuration
    std::wstring cfg_path;
    if (instance.name == L"BlueStacks") {
        cfg_path = instance.install_dir + L"\\bluestacks.conf";
    } else if (instance.name == L"MSI App Player") {
        cfg_path = instance.install_dir + L"\\config.ini";
    } else {
        return Result<void>::Err("Unsupported emulator for configuration write.");
    }
    // Load existing content to preserve unknown keys
    std::unordered_map<std::wstring, std::wstring> map;
    if (file_exists(cfg_path)) {
        std::ifstream in(cfg_path, std::ios::binary);
        std::stringstream buf; buf << in.rdbuf();
        auto parse_res = parse_ini(utf8_to_w(buf.str()));
        if (parse_res.is_ok()) map = parse_res.unwrap();
    }
    // Update known keys
    map[L"cpu_core_count"] = std::to_wstring(instance.configured_cpu_cores);
    map[L"memory_size_mb"] = std::to_wstring(instance.configured_ram_mb);
    if (!instance.rendering_mode.empty()) map[L"renderer"] = instance.rendering_mode;
    if (!instance.graphics_backend.empty()) map[L"graphics_backend"] = instance.graphics_backend;
    // Serialize back to UTF‑8 string
    std::wstring serialized = serialize_ini(map);
    // Write backup first
    auto backup_res = backup_configuration(instance);
    if (backup_res.is_err()) {
        Logger::instance().log(Logger::Level::Warn, L"Backup before write failed: " + std::wstring(backup_res.error().begin(), backup_res.error().end()));
        // Continue anyway – user consent assumed earlier.
    }
    // Write file
    std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
    if (!out) return Result<void>::Err("Failed to open configuration file for writing.");
    out << w2utf8(serialized);
    Logger::instance().log(Logger::Level::Info, L"Wrote configuration for " + instance.name + L" to " + cfg_path);
    return Result<void>::Ok();
}

// ---------------------------------------------------------------------
// Simple INI parser (key=value, ignore comments starting with # or ;)
// ---------------------------------------------------------------------
Result<EmulatorOptimizer::IniMap> EmulatorOptimizer::parse_ini(const std::wstring& content) {
    IniMap map;
    std::wstringstream ss(content);
    std::wstring line;
    while (std::getline(ss, line)) {
        // Trim whitespace
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](wchar_t ch){ return !iswspace(ch); }));
        line.erase(std::find_if(line.rbegin(), line.rend(), [](wchar_t ch){ return !iswspace(ch); }).base(), line.end());
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        // Trim both sides
        auto trim = [](std::wstring& s){
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](wchar_t ch){ return !iswspace(ch); }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [](wchar_t ch){ return !iswspace(ch); }).base(), s.end());
        };
        trim(key); trim(val);
        map[key] = val;
    }
    return Result<IniMap>::Ok(map);
}

std::wstring EmulatorOptimizer::serialize_ini(const IniMap& map) {
    std::wstringstream ss;
    for (const auto& kv : map) {
        ss << kv.first << L"=" << kv.second << L"\n";
    }
    return ss.str();
}

// ---------------------------------------------------------------------
// Performance analysis (lightweight – use PDH for CPU, GetProcessMemoryInfo for RAM)
// ---------------------------------------------------------------------
Result<EmulatorOptimizer::PerformanceMetrics> EmulatorOptimizer::analyze_performance(const EmulatorInstance& instance) {
    if (!instance.is_running || instance.pid == 0) {
        return Result<PerformanceMetrics>::Err("Emulator instance is not running.");
    }
    PerformanceMetrics pm{};
    // CPU usage via GetProcessTimes over a short interval (simplified)
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, instance.pid);
    if (!hProc) {
        return Result<PerformanceMetrics>::Err("Failed to open process handle for CPU measurement.");
    }
    FILETIME ftCreation, ftExit, ftKernel1, ftUser1;
    GetProcessTimes(hProc, &ftCreation, &ftExit, &ftKernel1, &ftUser1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // short sample window
    FILETIME ftKernel2, ftUser2;
    GetProcessTimes(hProc, &ftCreation, &ftExit, &ftKernel2, &ftUser2);
    ULONGLONG k1 = ((ULONGLONG)ftKernel1.dwHighDateTime << 32) | ftKernel1.dwLowDateTime;
    ULONGLONG u1 = ((ULONGLONG)ftUser1.dwHighDateTime << 32) | ftUser1.dwLowDateTime;
    ULONGLONG k2 = ((ULONGLONG)ftKernel2.dwHighDateTime << 32) | ftKernel2.dwLowDateTime;
    ULONGLONG u2 = ((ULONGLONG)ftUser2.dwHighDateTime << 32) | ftUser2.dwLowDateTime;
    ULONGLONG delta = (k2 + u2) - (k1 + u1);
    // 100‑ns units -> seconds
    double seconds = static_cast<double>(delta) / 1e7;
    SYSTEM_INFO sysInfo{}; GetSystemInfo(&sysInfo);
    pm.cpu_usage_percent = (seconds / 0.2) * 100.0 / static_cast<double>(sysInfo.dwNumberOfProcessors);
    // RAM usage via GetProcessMemoryInfo (psapi)
    PROCESS_MEMORY_COUNTERS_EX mem{};
    if (GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS*)&mem, sizeof(mem))) {
        pm.ram_usage_mb = static_cast<double>(mem.WorkingSetSize) / (1024.0 * 1024.0);
    }
    CloseHandle(hProc);
    // GPU usage, FPS, frame time – not directly accessible without emulator‑specific APIs.
    // We'll set placeholder values and let UI indicate "unavailable".
    pm.gpu_usage_percent = -1.0;
    pm.fps_average = -1.0;
    pm.fps_1_percent_low = -1.0;
    pm.frame_time_ms = -1.0;
    return Result<PerformanceMetrics>::Ok(pm);
}

// ---------------------------------------------------------------------
// Backup / restore helpers
// ---------------------------------------------------------------------
Result<void> EmulatorOptimizer::backup_configuration(const EmulatorInstance& instance) {
    std::wstring src;
    if (instance.name == L"BlueStacks") src = instance.install_dir + L"\\bluestacks.conf";
    else if (instance.name == L"MSI App Player") src = instance.install_dir + L"\\config.ini";
    else return Result<void>::Err("Unsupported emulator for backup.");
    if (!file_exists(src)) {
        return Result<void>::Err("Source configuration file not found for backup.");
    }
    std::wstring ts = timestamp();
    std::wstring dest_dir = _backup_root + L"\\" + instance.name + L"_" + ts;
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) return Result<void>::Err("Failed to create backup folder: " + ec.message());
    std::wstring dest = dest_dir + L"\\" + std::filesystem::path(src).filename().wstring();
    try {
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
    } catch (...) { return Result<void>::Err("Exception during file copy for backup."); }
    if (ec) return Result<void>::Err("Failed to copy config file: " + ec.message());
    Logger::instance().log(Logger::Level::Info, L"Backup created for " + instance.name + L" at " + dest);
    return Result<void>::Ok();
}

Result<void> EmulatorOptimizer::restore_latest_backup(EmulatorInstance& instance) {
    // Find latest backup directory for this emulator
    namespace fs = std::filesystem;
    std::vector<fs::directory_entry> dirs;
    std::wstring prefix = instance.name + L"_";
    for (auto& de : fs::directory_iterator(_backup_root)) {
        if (de.is_directory()) {
            std::wstring name = de.path().filename().wstring();
            if (name.rfind(prefix, 0) == 0) dirs.push_back(de);
        }
    }
    if (dirs.empty()) return Result<void>::Err("No backup directories found for this emulator.");
    // Sort descending (newest first)
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b){ return a.path().filename().wstring() > b.path().filename().wstring(); });
    // Assume the config file has the same name as during backup
    std::wstring config_name = (instance.name == L"BlueStacks") ? L"bluestacks.conf" : L"config.ini";
    for (const auto& d : dirs) {
        std::wstring backup_file = d.path().wstring() + L"\\" + config_name;
        if (file_exists(backup_file)) {
            std::wstring target = (instance.name == L"BlueStacks") ? instance.install_dir + L"\\bluestacks.conf" : instance.install_dir + L"\\config.ini";
            std::error_code ec;
            std::filesystem::copy_file(backup_file, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) continue; // try earlier backup if copy failed
            Logger::instance().log(Logger::Level::Info, L"Restored backup for " + instance.name + L" from " + backup_file);
            return Result<void>::Ok();
        }
    }
    return Result<void>::Err("No suitable backup file found for restore.");
}

// ---------------------------------------------------------------------
// Profile application – respects hardware limits and never forces unsupported values
// ---------------------------------------------------------------------
Result<void> EmulatorOptimizer::apply_profile(EmulatorInstance& instance, EmulatorProfile profile) {
    // Gather hardware caps
    SYSTEM_INFO sysInfo{}; GetSystemInfo(&sysInfo);
    int max_cores = sysInfo.dwNumberOfProcessors;
    MEMORYSTATUSEX mem{}; mem.dwLength = sizeof(mem); GlobalMemoryStatusEx(&mem);
    int total_ram_mb = static_cast<int>(mem.ullTotalPhys / (1024 * 1024));

    // Helper to clamp values safely
    auto clamp = [](int value, int minv, int maxv) { return std::max(minv, std::min(value, maxv)); };

    // Load current configuration (if not already loaded)
    Result<void> cfg_res = read_configuration(instance);
    if (cfg_res.is_err()) {
        // Continue with defaults if reading fails – we still want to apply safe profile.
        Logger::instance().log(Logger::Level::Warn, L"Failed to read current config for profile apply: " + std::wstring(cfg_res.error().begin(), cfg_res.error().end()));
    }

    // Choose target values per profile
    switch (profile) {
        case EmulatorProfile::LowEndPC:
            instance.configured_cpu_cores = clamp(max_cores / 2, 1, max_cores);
            instance.configured_ram_mb = clamp(total_ram_mb / 3, 512, total_ram_mb);
            instance.rendering_mode = L"OpenGL"; // safer on low‑end GPUs
            instance.graphics_backend = L"opengl";
            break;
        case EmulatorProfile::CompetitiveGaming:
            instance.configured_cpu_cores = clamp(max_cores - 1, 2, max_cores);
            instance.configured_ram_mb = clamp(total_ram_mb / 2, 1024, total_ram_mb);
            instance.rendering_mode = L"DirectX";
            instance.graphics_backend = L"dx12"; // attempt modern DX if supported
            break;
        case EmulatorProfile::Streaming:
            instance.configured_cpu_cores = clamp(max_cores - 2, 2, max_cores);
            instance.configured_ram_mb = clamp(total_ram_mb / 2, 1024, total_ram_mb);
            instance.rendering_mode = L"DirectX";
            instance.graphics_backend = L"dx11"; // stable baseline for encoding
            break;
        case EmulatorProfile::Custom:
            // Caller will have already set fields – do nothing.
            break;
        default:
            return Result<void>::Err("Unsupported profile.");
    }
    // Safety checks – never exceed hardware limits or set negative values.
    if (instance.configured_cpu_cores > max_cores) instance.configured_cpu_cores = max_cores;
    if (instance.configured_ram_mb > total_ram_mb) instance.configured_ram_mb = total_ram_mb;
    if (instance.configured_cpu_cores < 1) instance.configured_cpu_cores = 1;
    if (instance.configured_ram_mb < 256) instance.configured_ram_mb = 256;

    // Write back configuration (the UI must have obtained user consent before calling this)
    Result<void> write_res = write_configuration(instance);
    if (write_res.is_err()) {
        return Result<void>::Err("Failed to write configuration after applying profile: " + write_res.error());
    }
    Logger::instance().log(Logger::Level::Info, L"Applied profile " + std::to_wstring(static_cast<int>(profile)) + L" to " + instance.name);
    return Result<void>::Ok();
}

// ---------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------
std::optional<std::wstring> EmulatorOptimizer::read_file_utf8(const std::wstring& path) {
    if (!file_exists(path)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    std::stringstream buf; buf << in.rdbuf();
    return utf8_to_w(buf.str());
}

Result<void> EmulatorOptimizer::write_file_utf8(const std::wstring& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Result<void>::Err("Failed to open file for writing.");
    out << content;
    return Result<void>::Ok();
}

std::wstring EmulatorOptimizer::generate_timestamp() const { return timestamp(); }

bool EmulatorOptimizer::is_path_allowed(const std::wstring& path) const {
    // Only allow paths that are under the known install directories of the detected instances.
    // For simplicity, accept any path that contains "BlueStacks" or "MSI APP Player".
    return (path.find(L"BlueStacks") != std::wstring::npos) || (path.find(L"MSI APP Player") != std::wstring::npos);
}
