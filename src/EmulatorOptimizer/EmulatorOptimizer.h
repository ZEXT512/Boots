#pragma once
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <windows.h>

#include "../common/Result.h"
#include "../common/Logger.h"

/**
 * Information about a detected emulator instance.
 */
struct EmulatorInstance {
    std::wstring name;               // "MSI App Player" or "BlueStacks"
    std::wstring version;            // e.g. "5.13.0"
    std::wstring install_dir;        // Root installation directory
    DWORD pid;                       // Process ID if running, 0 otherwise
    int configured_cpu_cores;        // Number of CPU cores allocated (from config)
    int configured_ram_mb;           // RAM allocation in MB (from config)
    std::wstring rendering_mode;     // e.g. "DirectX", "OpenGL"
    std::wstring graphics_backend;   // e.g. "dx11", "dx12", "opengl"
    bool is_running;                // true if a process is active
};

/**
 * Supported performance profiles.
 */
enum class EmulatorProfile {
    LowEndPC,
    CompetitiveGaming,
    Streaming,
    Custom
};

/**
 * EmulatorOptimizer – central engine for detecting, analyzing and safely configuring
 * MSI App Player and BlueStacks emulators.
 *
 *   * initialise()        – prepares internal structures, creates backup folder.
 *   * detect_installed()  – returns a list of installed emulators (name, version, path).
 *   * detect_running()    – augments the list with running process IDs and status.
 *   * read_configuration() – parses the emulator's configuration files and fills
 *                             `configured_cpu_cores`, `configured_ram_mb`, etc.
 *   * analyze_performance() – gathers lightweight metrics (CPU, RAM, GPU usage)
 *                              via Performance Counters when the emulator is running.
 *   * apply_profile()      – writes a safe configuration profile (does not exceed
 *                             hardware limits, never forces unsupported values).
 *   * backup_configuration() – copies the current config files to a timestamped backup.
 *   * restore_backup()     – restores the most recent backup for a given emulator.
 */
class EmulatorOptimizer {
public:
    Result<void> initialize();
    Result<void> shutdown();

    // Detection
    Result<std::vector<EmulatorInstance>> detect_installed();
    Result<std::vector<EmulatorInstance>> detect_running(const std::vector<EmulatorInstance>& installed);

    // Configuration I/O
    Result<void> read_configuration(EmulatorInstance& instance);
    Result<void> write_configuration(const EmulatorInstance& instance);

    // Performance analysis (lightweight, optional)
    struct PerformanceMetrics {
        double cpu_usage_percent;   // overall CPU usage of the emulator process
        double ram_usage_mb;        // RAM used by process
        double gpu_usage_percent;   // approximated via GPU engine counters (optional)
        double fps_average;         // placeholder – to be filled by external benchmark tool
        double fps_1_percent_low;   // placeholder
        double frame_time_ms;        // average frame time
    };
    Result<PerformanceMetrics> analyze_performance(const EmulatorInstance& instance);

    // Profiles
    Result<void> apply_profile(EmulatorInstance& instance, EmulatorProfile profile);

    // Backup / restore
    Result<void> backup_configuration(const EmulatorInstance& instance);
    Result<void> restore_latest_backup(EmulatorInstance& instance);

private:
    std::wstring _backup_root; // %ProgramData%\BOOTFPS\emulator_backups

    // Helper helpers
    std::optional<std::wstring> read_file_utf8(const std::wstring& path);
    Result<void> write_file_utf8(const std::wstring& path, const std::string& content);
    std::wstring generate_timestamp() const;
    bool is_path_allowed(const std::wstring& path) const; // ensure we only touch known config locations

    // Parsing simple INI‑style config files (key=value lines, ignore comments)
    using IniMap = std::unordered_map<std::wstring, std::wstring>;
    Result<IniMap> parse_ini(const std::wstring& content);
    std::wstring serialize_ini(const IniMap& map);

    // Emulator‑specific helpers
    Result<EmulatorInstance> probe_msi(const std::wstring& base_dir);
    Result<EmulatorInstance> probe_bluestacks(const std::wstring& base_dir);
};
