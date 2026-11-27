#pragma once

#include <string>
#include "../common/Result.h"

namespace WindowsSystemAnalyzer {

/**
 * Read-only Windows system information used by the BOOTFPS dashboard.
 * Every getter is safe to call from any thread and never throws.
 */
class SystemInfoProvider {
public:
    Result<void> initialize();
    Result<void> shutdown();

    // OS / identity
    std::string os_version();   // e.g. "Windows 11 (10.0.26100)"
    std::string hostname();
    std::string user_name();
    bool is_admin();

    // Hardware summary
    std::string cpu_name();
    unsigned int logical_cores();
    unsigned long long total_ram_mb();
    unsigned long long uptime_seconds();
};

} // namespace WindowsSystemAnalyzer
