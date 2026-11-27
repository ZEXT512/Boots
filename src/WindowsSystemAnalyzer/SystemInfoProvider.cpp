// BOOTFPS — WindowsSystemAnalyzer implementation.
#include "SystemInfoProvider.h"
#include "../common/Logger.h"

#include <windows.h>

#include <string>
#include <vector>

namespace {

std::string narrow(const wchar_t* text) {
    if (!text) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    out.resize(static_cast<size_t>(needed - 1));
    return out;
}

std::string narrow(const std::wstring& text) {
    return narrow(text.c_str());
}

// RtlGetVersion reports the true OS build even when the executable has no
// compatibility manifest (GetVersionEx would lie).
struct RtlOsVersionInfo {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};

bool read_registry_string(HKEY root, const std::wstring& subkey,
                          const std::wstring& value, std::wstring& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG rc = RegQueryValueExW(key, value.c_str(), nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    rc = RegQueryValueExW(key, value.c_str(), nullptr, &type,
                          reinterpret_cast<LPBYTE>(buffer.data()), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    out.assign(buffer.data());
    return true;
}

} // namespace

namespace WindowsSystemAnalyzer {

Result<void> SystemInfoProvider::initialize() {
    Logger::instance().log(Logger::Level::Info, L"SystemInfoProvider initialized.");
    return Result<void>::Ok();
}

Result<void> SystemInfoProvider::shutdown() {
    Logger::instance().log(Logger::Level::Info, L"SystemInfoProvider shutdown.");
    return Result<void>::Ok();
}

std::string SystemInfoProvider::os_version() {
    RtlOsVersionInfo info{};
    info.dwOSVersionInfoSize = sizeof(info);

    using RtlGetVersionFn = LONG(WINAPI*)(RtlOsVersionInfo*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion && rtlGetVersion(&info) == 0) {
            std::string name = "Windows";
            if (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000) {
                name = "Windows 11";
            } else if (info.dwMajorVersion == 10) {
                name = "Windows 10";
            } else if (info.dwMajorVersion == 6 && info.dwMinorVersion == 3) {
                name = "Windows 8.1";
            } else if (info.dwMajorVersion == 6 && info.dwMinorVersion == 2) {
                name = "Windows 8";
            } else if (info.dwMajorVersion == 6 && info.dwMinorVersion == 1) {
                name = "Windows 7";
            }
            return name + " (" + std::to_string(info.dwMajorVersion) + "." +
                   std::to_string(info.dwMinorVersion) + "." +
                   std::to_string(info.dwBuildNumber) + ")";
        }
    }
    return "Windows (unknown build)";
}

std::string SystemInfoProvider::hostname() {
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(buffer, &size)) return std::string(buffer, size);
    return "unknown-host";
}

std::string SystemInfoProvider::user_name() {
    char buffer[256] = {};
    DWORD size = 256;
    if (GetUserNameA(buffer, &size)) return std::string(buffer);
    return "unknown-user";
}

bool SystemInfoProvider::is_admin() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, size, &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

std::string SystemInfoProvider::cpu_name() {
    std::wstring name;
    if (read_registry_string(HKEY_LOCAL_MACHINE,
                             L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                             L"ProcessorNameString", name)) {
        // Trim leading/trailing spaces that the registry value often carries.
        const auto first = name.find_first_not_of(L' ');
        const auto last = name.find_last_not_of(L' ');
        if (first != std::wstring::npos) name = name.substr(first, last - first + 1);
        return narrow(name);
    }
    return "Unknown CPU";
}

unsigned int SystemInfoProvider::logical_cores() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
}

unsigned long long SystemInfoProvider::total_ram_mb() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return 0;
    return memory.ullTotalPhys / (1024ull * 1024ull);
}

unsigned long long SystemInfoProvider::uptime_seconds() {
    return static_cast<unsigned long long>(GetTickCount64() / 1000ull);
}

} // namespace WindowsSystemAnalyzer
