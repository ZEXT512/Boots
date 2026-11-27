#pragma once
#include <string>
#include <string_view>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <source_location>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class Logger {
public:
    enum class Level { Trace, Debug, Info, Warn, Error, Critical };
    static Logger& instance() {
        static Logger logger;
        return logger;
    }
    void log(Level lvl, std::string_view msg, const std::source_location& loc = std::source_location::current()) {
        write(lvl, msg, loc);
    }

    // Modules in BOOTFPS build their diagnostics from wide strings (paths, process
    // names). Accept them here instead of forcing every call site to convert.
    void log(Level lvl, std::wstring_view msg, const std::source_location& loc = std::source_location::current()) {
#if defined(_WIN32)
        std::string utf8;
        if (!msg.empty()) {
            int len = ::WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                utf8.resize(static_cast<size_t>(len));
                ::WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), utf8.data(), len, nullptr, nullptr);
            }
        }
        write(lvl, utf8, loc);
#else
        std::string narrow;
        narrow.reserve(msg.size());
        for (wchar_t c : msg) narrow.push_back(c <= 0x7F ? static_cast<char>(c) : '?');
        write(lvl, narrow, loc);
#endif
    }

private:
    void write(Level lvl, std::string_view msg, const std::source_location& loc) {
        std::lock_guard<std::mutex> lock(_mtx);
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &tt);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << levelToString(lvl) << "] " << msg << " (" << loc.file_name() << ":" << loc.line() << ")\n";
        std::string line = oss.str();
        // Write to file (ProgramData\BOOTFPS\logs)
        const char* programData = getenv("ProgramData");
        std::filesystem::path logDir = std::filesystem::path(programData ? programData : "C:\\ProgramData") / "BOOTFPS" / "logs";
        std::filesystem::create_directories(logDir);
        std::filesystem::path logFile = logDir / "bootfps.log";
        std::ofstream out(logFile, std::ios::app);
        out << line;
    }

    std::mutex _mtx;
    Logger() = default;
    const char* levelToString(Level lvl) const {
        switch (lvl) {
            case Level::Trace: return "Trace";
            case Level::Debug: return "Debug";
            case Level::Info: return "Info";
            case Level::Warn: return "Warn";
            case Level::Error: return "Error";
            case Level::Critical: return "Critical";
        }
        return "Unknown";
    }
};
