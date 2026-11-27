#pragma once
#include <string>
#include <vector>
#include <windows.h>
#include <guiddef.h>

#include "../common/Result.h"
#include "../common/Logger.h"

/**
 * Simple representation of a Windows power scheme.
 */
struct PowerPlan {
    GUID guid;                 // Power scheme GUID
    std::wstring name;         // Friendly name
    bool is_active = false;    // True if this is the currently active plan
};

/**
 * Power optimization profiles – these map to well‑known Windows power schemes.
 * The module will switch to the closest matching built‑in plan, creating a copy
 * if necessary, without overriding manufacturer limits.
 */
enum class PowerProfile {
    Balanced,
    Performance,
    Emulator,
    Streaming,
    Custom
};

/**
 * PowerOptimizer – central engine for detecting, switching and safely tweaking
 * Windows power settings for gaming, emulation and streaming workloads.
 *
 *   * initialize() – prepares internal state, creates backup folder.
 *   * get_current_plan() – returns the active PowerPlan.
 *   * list_available_plans() – enumerates all installed schemes.
 *   * set_plan(const PowerPlan&) – activates a given scheme.
 *   * apply_profile(PowerProfile) – creates/adjusts a scheme for the selected profile.
 *   * diagnose() – returns a list of human‑readable warnings about CPU frequency
 *                   limits, power‑management restrictions and conflicting settings.
 *   * benchmark_before() / benchmark_after() – run a quick CPU‑bound loop and
 *                                            return timing (seconds). These are
 *                                            used to compare performance impact.
 *   * restore_original() – restores the power plan that was active at module start.
 */
class PowerOptimizer {
public:
    Result<void> initialize();
    Result<void> shutdown();

    // Power plan handling
    Result<PowerPlan> get_current_plan() const;
    Result<std::vector<PowerPlan>> list_available_plans() const;
    Result<void> set_plan(const PowerPlan& plan);

    // Profile handling
    Result<void> apply_profile(PowerProfile profile);

    // Diagnostics
    std::vector<std::string> diagnose() const; // returns warnings/notes

    // Benchmark (simple CPU loop)
    Result<double> benchmark(); // runs a short CPU stress test and returns seconds

    // Restore the original plan saved at initialise()
    Result<void> restore_original();

private:
    GUID _original_guid{}; // saved active scheme GUID at initialise()
    std::wstring _backup_dir; // %ProgramData%\BOOTFPS\power_backups

    // Helpers for GUID <-> string conversion
    static std::wstring guid_to_wstring(const GUID& guid);
    static GUID wstring_to_guid(const std::wstring& str);

    // Helper to copy an existing scheme (creates a new GUID)
    Result<GUID> duplicate_scheme(const GUID& source_guid, const std::wstring& new_name);

    // Low‑level wrappers around Power API (error handling, logging)
    static Result<GUID> get_active_scheme();
    static Result<void> set_active_scheme(const GUID& guid);
    static Result<std::vector<PowerPlan>> enumerate_schemes();
    static Result<std::wstring> get_scheme_name(const GUID& guid);
    static Result<void> set_scheme_name(const GUID& guid, const std::wstring& name);
};
