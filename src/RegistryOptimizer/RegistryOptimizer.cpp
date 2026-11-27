#include "RegistryOptimizer.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <algorithm>

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
    std::wstring timestamp_str() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t buf[64];
        swprintf(buf, 64, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::wstring(buf);
    }
    std::wstring risk_to_string(RiskLevel r) {
        switch (r) {
            case RiskLevel::Low: return L"Low";
            case RiskLevel::Medium: return L"Medium";
            case RiskLevel::High: return L"High";
            case RiskLevel::Critical: return L"Critical";
        }
        return L"Unknown";
    }
    std::wstring datatype_to_string(RegDataType t) {
        switch (t) {
            case RegDataType::DWord: return L"DWORD";
            case RegDataType::QWord: return L"QWORD";
            case RegDataType::String: return L"REG_SZ";
            case RegDataType::ExpandString: return L"REG_EXPAND_SZ";
            case RegDataType::Binary: return L"REG_BINARY";
            default: return L"UNKNOWN";
        }
    }
}

Result<void> RegistryOptimizer::initialize() {
    _backup_dir = program_data_path() + L"\\BOOTFPS\\registry_backups";
    std::error_code ec;
    std::filesystem::create_directories(_backup_dir, ec);
    if (ec) {
        return Result<void>::Err("Failed to create backup directory: " + ec.message());
    }
    load_managed_keys();
    Logger::instance().log(Logger::Level::Info, L"RegistryOptimizer initialized with " + std::to_wstring(_managed_keys.size()) + L" managed keys.");
    return Result<void>::Ok();
}

Result<void> RegistryOptimizer::shutdown() {
    Logger::instance().log(Logger::Level::Info, L"RegistryOptimizer shutdown.");
    return Result<void>::Ok();
}

void RegistryOptimizer::load_managed_keys() {
    // For demonstration we populate a few well‑known performance keys.
    // In a real product this would be a large data table loaded from a resource file.
    _managed_keys.clear();
    // Example: Disable mouse acceleration – SystemParameters already does this, but registry key exists.
    _managed_keys.push_back({
        HKEY_CURRENT_USER,
        L"Control Panel\\Mouse",
        L"MouseSpeed",
        RegDataType::String,
        L"0", // proposed: 0 = no acceleration
        L"Disables mouse acceleration for consistent pointer movement in games.",
        L"Windows 10/11, any hardware",
        RiskLevel::Low,
        OptCategory::MouseKeyboard
    });
    // Example: Power throttling mode for games (system setting under Power Settings)
    _managed_keys.push_back({
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling",
        L"PowerThrottlingMode",
        RegDataType::DWord,
        L"0", // 0 = disabled (best performance)
        L"Disables power throttling which can limit CPU/GPU burst performance in games.",
        L"Windows 10 version 1809+, gaming laptops",
        RiskLevel::Medium,
        OptCategory::GamingPerformance
    });
    // Example: Multimedia scheduling – disable multimedia class scheduling (MSC)
    _managed_keys.push_back({
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\MultimediaClassScheduler",
        L"EnableMMCSS",
        RegDataType::DWord,
        L"0",
        L"Turns off MMCSS which can improve latency for audio‑intensive apps.",
        L"Windows 10/11, audio workstations",
        RiskLevel::Medium,
        OptCategory::MultimediaScheduling
    });
    // Example: Power plan "High performance" registry tweak (not exhaustive)
    _managed_keys.push_back({
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerSettings\\{54533251-82be-4824-96c1-47b60b740d00}\\{a1841308-3541-4fab-bc81-f71556f20b4a}",
        L"Attributes",
        RegDataType::DWord,
        L"2", // 2 = hidden from UI, but we can set to 2 to enable "Turn off display after" faster etc.
        L"Ensures certain power settings are exposed for fine‑tuning.",
        L"Windows 10/11, desktops/laptops",
        RiskLevel::Low,
        OptCategory::PowerManagement
    });
}

Result<void> RegistryOptimizer::scan() {
    _entries.clear();
    for (const auto& mk : _managed_keys) {
        RegistryEntry entry{};
        entry.root = mk.root;
        entry.subkey = mk.subkey;
        entry.value_name = mk.value_name;
        entry.type = mk.type;
        entry.proposed_str = mk.proposed_str;
        entry.explanation = mk.explanation;
        entry.compatibility = mk.compatibility;
        entry.risk = mk.risk;
        entry.is_modified = false;
        entry.is_valid = true;
        // Read current value
        RegDataType actual_type = RegDataType::Unknown;
        auto cur_res = read_value(mk.root, mk.subkey, mk.value_name, actual_type);
        if (cur_res.is_err()) {
            entry.is_valid = false;
            entry.current_str = std::nullopt;
            Logger::instance().log(Logger::Level::Warn, L"Failed reading registry key: " + mk.subkey + L"\\" + mk.value_name);
        } else {
            entry.current_str = cur_res.unwrap();
            if (entry.current_str) {
                entry.type = actual_type; // use actual type if differs
                if (entry.proposed_str != *entry.current_str) {
                    entry.is_modified = true;
                }
            } else {
                // The value does not exist yet. Applying the proposal creates it,
                // so surface the entry as a pending change instead of invalid.
                entry.is_modified = entry.proposed_str.has_value();
            }
        }
        _entries.push_back(std::move(entry));
    }
    Logger::instance().log(Logger::Level::Info, L"Registry scan completed, entries=" + std::to_wstring(_entries.size()));
    return Result<void>::Ok();
}

Result<std::optional<std::wstring>> RegistryOptimizer::read_value(HKEY root, const std::wstring& subkey, const std::wstring& name, RegDataType& out_type) {
    HKEY hKey;
    LONG rc = RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey);
    if (rc == ERROR_FILE_NOT_FOUND) {
        // Key absent: report "no current value" so the proposal can create it.
        return Result<std::optional<std::wstring>>::Ok(std::nullopt);
    }
    if (rc != ERROR_SUCCESS) {
        return Result<std::optional<std::wstring>>::Err("Unable to open registry key for read.");
    }
    DWORD type = 0;
    DWORD dataSize = 0;
    rc = RegQueryValueExW(hKey, name.c_str(), nullptr, &type, nullptr, &dataSize);
    if (rc == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(hKey);
        return Result<std::optional<std::wstring>>::Ok(std::nullopt);
    }
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return Result<std::optional<std::wstring>>::Err("Failed to query registry value size.");
    }
    std::vector<BYTE> buffer(dataSize);
    rc = RegQueryValueExW(hKey, name.c_str(), nullptr, &type, buffer.data(), &dataSize);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        return Result<std::optional<std::wstring>>::Err("Failed to read registry value.");
    }
    // Determine data type and convert to string for display.
    switch (type) {
        case REG_DWORD: {
            out_type = RegDataType::DWord;
            DWORD val = *reinterpret_cast<DWORD*>(buffer.data());
            return Result<std::optional<std::wstring>>::Ok(std::to_wstring(val));
        }
        case REG_QWORD: {
            out_type = RegDataType::QWord;
            ULONGLONG val = *reinterpret_cast<ULONGLONG*>(buffer.data());
            return Result<std::optional<std::wstring>>::Ok(std::to_wstring(val));
        }
        case REG_SZ: {
            out_type = RegDataType::String;
            std::wstring val(reinterpret_cast<wchar_t*>(buffer.data()), dataSize / sizeof(wchar_t) - 1);
            return Result<std::optional<std::wstring>>::Ok(val);
        }
        case REG_EXPAND_SZ: {
            out_type = RegDataType::ExpandString;
            std::wstring val(reinterpret_cast<wchar_t*>(buffer.data()), dataSize / sizeof(wchar_t) - 1);
            return Result<std::optional<std::wstring>>::Ok(val);
        }
        case REG_BINARY: {
            out_type = RegDataType::Binary;
            // Represent binary as hex string for display.
            std::wstringstream ss;
            for (size_t i = 0; i < buffer.size(); ++i) {
                ss << std::hex << std::uppercase << (int)buffer[i];
                if (i + 1 < buffer.size()) ss << L" ";
            }
            return Result<std::optional<std::wstring>>::Ok(ss.str());
        }
        default:
            out_type = RegDataType::Unknown;
            return Result<std::optional<std::wstring>>::Ok(std::nullopt);
    }
}

Result<void> RegistryOptimizer::write_value(HKEY root, const std::wstring& subkey, const std::wstring& name, RegDataType type, const std::wstring& data) {
    HKEY hKey;
    LONG rc = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("Unable to open/create registry key for write.");
    }
    switch (type) {
        case RegDataType::DWord: {
            DWORD val = static_cast<DWORD>(std::stoul(data));
            rc = RegSetValueExW(hKey, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
            break;
        }
        case RegDataType::QWord: {
            ULONGLONG val = static_cast<ULONGLONG>(std::stoull(data));
            rc = RegSetValueExW(hKey, name.c_str(), 0, REG_QWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
            break;
        }
        case RegDataType::String: {
            rc = RegSetValueExW(hKey, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()), static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
            break;
        }
        case RegDataType::ExpandString: {
            rc = RegSetValueExW(hKey, name.c_str(), 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(data.c_str()), static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
            break;
        }
        case RegDataType::Binary: {
            // Expect hex string, convert to bytes (simple implementation)
            std::vector<BYTE> bytes;
            std::wstringstream ws(data);
            std::wstring token;
            while (ws >> token) {
                BYTE b = static_cast<BYTE>(std::stoi(token, nullptr, 16));
                bytes.push_back(b);
            }
            rc = RegSetValueExW(hKey, name.c_str(), 0, REG_BINARY, bytes.data(), static_cast<DWORD>(bytes.size()));
            break;
        }
        default:
            RegCloseKey(hKey);
            return Result<void>::Err("Unsupported data type for write.");
    }
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) {
        return Result<void>::Err("Failed to write registry value. Error code: " + std::to_string(rc));
    }
    return Result<void>::Ok();
}

bool RegistryOptimizer::is_path_allowed(HKEY root, const std::wstring& subkey) const {
    // Very basic safe‑list: we only allow paths that are present in _managed_keys.
    for (const auto& mk : _managed_keys) {
        if (mk.root == root && mk.subkey == subkey) {
            return true;
        }
    }
    return false;
}

Result<std::string> RegistryOptimizer::apply_change(const RegistryEntry& entry) {
    if (!is_path_allowed(entry.root, entry.subkey)) {
        return Result<std::string>::Err("Attempted to modify a registry path not managed by BOOTFPS.");
    }
    // Backup current value before changing.
    json backup_entry;
    backup_entry["root"] = (entry.root == HKEY_CURRENT_USER) ? "HKEY_CURRENT_USER" : (entry.root == HKEY_LOCAL_MACHINE) ? "HKEY_LOCAL_MACHINE" : "UNKNOWN";
    backup_entry["subkey"] = std::string(entry.subkey.begin(), entry.subkey.end());
    backup_entry["value_name"] = std::string(entry.value_name.begin(), entry.value_name.end());
    backup_entry["current"] = entry.current_str ? std::string(entry.current_str->begin(), entry.current_str->end()) : "";
    backup_entry["timestamp"] = std::string(timestamp_str().begin(), timestamp_str().end());
    // Write backup file (single entry) – append to profile backup.
    std::wstring backup_file = _backup_dir + L"\\" + timestamp_str() + L".json";
    std::ofstream out(backup_file, std::ios::app);
    if (out) {
        out << backup_entry.dump() << "\n";
    }

    // Perform the write.
    Result<void> write_res = write_value(entry.root, entry.subkey, entry.value_name, entry.type, entry.proposed_str.value_or(L""));
    if (write_res.is_err()) {
        return Result<std::string>::Err("Failed to write registry value: " + write_res.error());
    }
    Logger::instance().log(Logger::Level::Info, L"Registry change applied: " + entry.subkey + L"\\" + entry.value_name);
    return Result<std::string>::Ok("Registry value applied successfully.");
}

Result<void> RegistryOptimizer::backup_profile() {
    json j = json::array();
    for (const auto& e : _entries) {
        json item;
        item["root"] = (e.root == HKEY_CURRENT_USER) ? "HKEY_CURRENT_USER" : (e.root == HKEY_LOCAL_MACHINE) ? "HKEY_LOCAL_MACHINE" : "UNKNOWN";
        item["subkey"] = std::string(e.subkey.begin(), e.subkey.end());
        item["value_name"] = std::string(e.value_name.begin(), e.value_name.end());
        item["type"] = std::string(datatype_to_string(e.type).begin(), datatype_to_string(e.type).end());
        item["current"] = e.current_str ? std::string(e.current_str->begin(), e.current_str->end()) : "";
        item["proposed"] = e.proposed_str ? std::string(e.proposed_str->begin(), e.proposed_str->end()) : "";
        item["explanation"] = std::string(e.explanation.begin(), e.explanation.end());
        item["compatibility"] = std::string(e.compatibility.begin(), e.compatibility.end());
        item["risk"] = std::string(risk_to_string(e.risk).begin(), risk_to_string(e.risk).end());
        j.push_back(item);
    }
    std::wstring file = _backup_dir + L"\\" + timestamp_str() + L"_profile.json";
    std::ofstream out(file);
    if (!out) {
        return Result<void>::Err("Failed to write registry profile backup.");
    }
    out << j.dump(4);
    Logger::instance().log(Logger::Level::Info, L"Registry profile backup saved to " + file);
    return Result<void>::Ok();
}

Result<std::string> RegistryOptimizer::restore_entry(const RegistryEntry& entry) {
    // Restore the value from backup – we locate the most recent backup for this key.
    // For simplicity we read the latest backup file in the directory.
    namespace fs = std::filesystem;
    std::vector<fs::directory_entry> files;
    for (const auto& de : fs::directory_iterator(_backup_dir)) {
        if (de.is_regular_file() && de.path().extension() == L".json") files.push_back(de);
    }
    if (files.empty()) {
        return Result<std::string>::Err("No backup files found.");
    }
    // Sort by filename (timestamp) descending.
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b){ return a.path().filename() > b.path().filename(); });
    for (const auto& f : files) {
        std::ifstream in(f.path());
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            try {
                json obj = json::parse(line);
                if (obj["subkey"] == std::string(entry.subkey.begin(), entry.subkey.end()) &&
                    obj["value_name"] == std::string(entry.value_name.begin(), entry.value_name.end())) {
                    std::string saved = obj["current"].get<std::string>();
                    // Convert back to wstring
                    std::wstring saved_ws(saved.begin(), saved.end());
                    Result<void> wres = write_value(entry.root, entry.subkey, entry.value_name, entry.type, saved_ws);
                    if (wres.is_err()) {
                        return Result<std::string>::Err("Failed to restore registry value: " + wres.error());
                    }
                    Logger::instance().log(Logger::Level::Info, L"Restored registry entry: " + entry.subkey + L"\\" + entry.value_name);
                    return Result<std::string>::Ok("Entry restored successfully.");
                }
            } catch (...) {
                continue; // ignore malformed lines
            }
        }
    }
    return Result<std::string>::Err("No matching backup entry found for the specified key.");
}

Result<void> RegistryOptimizer::restore_category(OptCategory cat) {
    // Iterate over entries belonging to the category and restore each.
    for (auto& e : _entries) {
        // Determine category from managed keys list matching subkey/value.
        auto it = std::find_if(_managed_keys.begin(), _managed_keys.end(), [&](const ManagedKey& mk){
            return mk.root == e.root && mk.subkey == e.subkey && mk.value_name == e.value_name;
        });
        if (it != _managed_keys.end() && it->category == cat) {
            auto res = restore_entry(e);
            if (res.is_err()) {
                Logger::instance().log(Logger::Level::Warn, L"Failed to restore entry " + e.subkey + L"\\" + e.value_name + L": " + std::wstring(res.error().begin(), res.error().end()));
                // Continue restoring other entries – not fatal
            }
        }
    }
    Logger::instance().log(Logger::Level::Info, L"Restoration of category completed.");
    return Result<void>::Ok();
}
