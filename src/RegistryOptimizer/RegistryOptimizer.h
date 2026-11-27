#pragma once
#include <string>
#include <vector>
#include <optional>
#include <windows.h>

#include "../common/Result.h"
#include "../common/Logger.h"

/**
 * Registry value data types we support.
 */
enum class RegDataType {
    DWord,
    QWord,
    String,
    ExpandString,
    Binary,
    Unknown
};

/**
 * Human‑readable risk level for a change.
 */
enum class RiskLevel {
    Low,
    Medium,
    High,
    Critical
};

/**
 * Represents a single registry setting that can be inspected and possibly tuned.
 */
struct RegistryEntry {
    HKEY root;                       // e.g. HKEY_CURRENT_USER
    std::wstring subkey;             // registry path relative to root
    std::wstring value_name;         // name of the value (empty for default)
    RegDataType type;                // data type
    std::optional<std::wstring> current_str; // current value as string (for display)
    std::optional<std::wstring> proposed_str; // proposed value as string
    std::wstring explanation;        // why this setting matters
    std::wstring compatibility;      // OS version info, hardware requirements etc.
    RiskLevel risk;                  // assessed risk of changing it
    bool is_modified = false;        // true if current != proposed
    bool is_valid = true;            // false if reading failed or value is unexpected
};

/**
 * Logical grouping of registry entries for a particular optimisation focus.
 */
enum class OptCategory {
    GamingPerformance,
    MouseKeyboard,
    MultimediaScheduling,
    PowerManagement,
    WindowsResponsiveness,
    EmulatorCompatibility,
    Unknown
};

/**
 * RegistryOptimizer – central engine for scanning, reporting and safely modifying
 * Windows registry performance‑related settings.
 *
 *   * initialise()    – loads built‑in safe‑list of keys to manage.
 *   * scan()          – populates a snapshot of current values.
 *   * get_entries()   – returns the snapshot for UI consumption.
 *   * apply_change()  – writes a proposed value after confirmation and backup.
 *   * backup_profile() – serialises current snapshot to a JSON file.
 *   * restore_entry() – restores a single entry from its backup.
 *   * restore_profile() – restores all entries belonging to a category.
 */
class RegistryOptimizer {
public:
    Result<void> initialize();
    Result<void> shutdown();

    // Scan the known set of performance‑related keys and fill _entries.
    Result<void> scan();
    const std::vector<RegistryEntry>& get_entries() const { return _entries; }

    // Apply a specific change – caller must have displayed the details and obtained user consent.
    Result<std::string> apply_change(const RegistryEntry& entry);

    // Backup the current snapshot to a JSON file (timestamped).
    Result<void> backup_profile();

    // Restore a single entry from the most recent backup.
    Result<std::string> restore_entry(const RegistryEntry& entry);

    // Restore all entries belonging to a category (e.g., GamingPerformance).
    Result<void> restore_category(OptCategory cat);

private:
    std::vector<RegistryEntry> _entries;
    std::wstring _backup_dir; // %ProgramData%\BOOTFPS\registry_backups

    // Helpers for reading/writing registry values.
    Result<std::optional<std::wstring>> read_value(HKEY root, const std::wstring& subkey, const std::wstring& name, RegDataType& out_type);
    Result<void> write_value(HKEY root, const std::wstring& subkey, const std::wstring& name, RegDataType type, const std::wstring& data);

    // Safety guard – returns false if the path is known to be dangerous.
    bool is_path_allowed(HKEY root, const std::wstring& subkey) const;

    // Internal list of keys we manage – populated in initialise().
    struct ManagedKey {
        HKEY root;
        std::wstring subkey;
        std::wstring value_name;
        RegDataType type;
        std::wstring proposed_str; // what we would like to set
        std::wstring explanation;
        std::wstring compatibility;
        RiskLevel risk;
        OptCategory category;
    };
    std::vector<ManagedKey> _managed_keys;

    // Load the static list of managed keys (hard‑coded for now).
    void load_managed_keys();
};
