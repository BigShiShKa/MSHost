#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <mutex>
#include <thread>
#include <filesystem>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

/* ===== Статусы Caddy ===== */
enum class CaddyStatus {
    Stopped,
    Starting,
    Running,
    Stopping
};

inline constexpr const char* caddy_status_text[] = {
    "Stopped", "Starting", "Running", "Stopping"
};

inline constexpr const wchar_t* caddy_status_text_wide[] = {
    L"Stopped", L"Starting", L"Running", L"Stopping"
};

inline std::ostream& operator<<(std::ostream& os, CaddyStatus s) {
    return os << caddy_status_text[static_cast<int>(s)];
}

inline std::wostream& operator<<(std::wostream& os, CaddyStatus s) {
    return os << caddy_status_text_wide[static_cast<int>(s)];
}


class CaddyManager {
public:
    explicit CaddyManager(const json& config_data);
    ~CaddyManager();

    CaddyManager(const CaddyManager&) = delete;
    CaddyManager& operator=(const CaddyManager&) = delete;

    void start();
    void stop();
    void restart();

    bool        is_running() const;
    CaddyStatus get_status() const;
    bool        is_enabled() const;

    void reload_config(const json& config_data);

private:
    struct Config {
        bool        enabled      = false;
        std::string exe_path;
        std::string config_path;
        std::string working_dir;

        bool is_valid() const {
            if (exe_path.empty() || config_path.empty() || working_dir.empty())
                return false;
            try {
                return fs::exists(exe_path) && fs::exists(config_path) && fs::is_directory(working_dir);
            } catch (const fs::filesystem_error&) {
                return false;
            }
        }
    } config_;

    void load_config(const json& config_data);
    bool validate_config();
    void monitor_process_exit();
    static std::string get_last_error_message(DWORD error_code);

    std::atomic<bool>        running_{false};
    std::atomic<CaddyStatus> status_{CaddyStatus::Stopped};

    PROCESS_INFORMATION procInfo_{};
    std::mutex          handle_mutex_;
    std::thread         monitor_thread_;
};
