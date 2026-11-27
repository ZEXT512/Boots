#pragma once
#include <string>
#include <windows.h>

/**
 * Simple data container for a running process.
 */
struct ProcessInfo {
    DWORD pid;                 // Process ID
    std::wstring name;         // Executable name (UTF‑16)
    double cpu_percent;       // CPU usage (0‑100)
    SIZE_T memory_bytes;       // Working set size (bytes)
    bool is_background;       // Heuristic: low CPU & window style
    bool is_high_cpu;         // cpu_percent > threshold (e.g., 20‑% )
    bool is_user_app;         // Heuristic: not a system service
};
