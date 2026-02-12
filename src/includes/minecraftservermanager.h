#pragma once

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#endif

#include <windows.h>

#include <atomic>
#include <string>
#include <mutex>
#include <ostream>
#include <thread>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

/* ===== Перечисление статусов ===== */
enum class ServerStatus {
    Stopped,
    Starting,
    Running,
    Stopping
};

inline constexpr const char*  status_text_narrow[] = {
    "Stopped", "Starting", "Running", "Stopping", "Error"
};
inline constexpr const wchar_t* status_text_wide[] = {
    L"Stopped", L"Starting", L"Running", L"Stopping", L"Error"
};

// Узкий поток
inline std::ostream& operator<<(std::ostream& os, ServerStatus s) {
    return os << status_text_narrow[static_cast<int>(s)];
}

// Широкий поток
inline std::wostream& operator<<(std::wostream& os, ServerStatus s) {
    return os << status_text_wide[static_cast<int>(s)];
}


class MinecraftServerManager {
public:
    MinecraftServerManager(const json& config_data);
    ~MinecraftServerManager();

    void start();
    void stop();

    bool         is_running() const;   // true, когда сервер «готов»
    ServerStatus get_status()  const;  // Текущий статус

    void send_command(const std::string& command);  // Передать консольную команду

private:
    /* Конфиги */
    struct Config {
        std::string java_path;
        std::vector<std::string> jvm_args_vec;
        std::string server_dir;
        std::string forge_args;
        std::string user_jvm_args;
        std::string full_command;

        DWORD stop_timeout_ms      = 20000;
        DWORD force_kill_timeout_ms = 5000;

        bool is_valid() const {
            if (java_path.empty() || server_dir.empty()) {
                return false;
            }
            try {
                if (!fs::exists(java_path) || !fs::exists(server_dir)) {
                    return false;
                }
            } catch (const fs::filesystem_error&) {
                return false;
            }
            return true;
        }
    } config_;

    void load_config(json config_data);

    /* Внутренние потоки */
    void read_output();            // Чтение stdout сервера
    void monitor_process_exit();   // Ожидание смерти процесса

    /* Вспомогалки */
    static std::string get_last_error_message(DWORD error_code);

    /* Состояние */
    std::atomic<bool>      running_{false};
    std::atomic<bool>      ready_{false};
    std::atomic<ServerStatus> status_{ServerStatus::Stopped};

    /* IPC-хендлы */
    HANDLE stdinPipe_ {nullptr};
    HANDLE readPipe_  {nullptr};
    PROCESS_INFORMATION procInfo_{};
    std::mutex pipe_mutex_;           // Защита записи в stdinPipe_
    std::mutex handle_mutex_;         // Защита закрытия хендлов

    /* Потоки */
    std::thread output_thread_;
    std::thread process_monitor_thread_;
};