#pragma once

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#endif

#include <windows.h>

#include <atomic>
#include <string>
#include <ostream>  
#include <thread>
#include <fstream>
#include <sstream>
#include "json.hpp"
//#include "rcon_client.h"
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
        std::vector<std::string> jvm_args_vec;  // Исходные аргументы
        std::string jvm_args;           // Преобразованная строка аргументов
        std::string server_dir;
        std::string forge_args;
        std::string user_jvm_args;
        std::string full_command;

        /* RCON конфигурация
        struct RCONConfig {
            bool enabled = false;
            std::string host = "127.0.0.1";
            int port = 25575;
            std::string password;
            int retry_interval = 5000; // ms
            int max_retries = 12;      // ~1 minute total
        } rcon;*/

        bool is_valid() const {
            // Проверяем, что основные пути существуют и не пусты
            if (java_path.empty() || server_dir.empty()) {
                return false;
            }
            
            // Проверяем существование файлов/директорий
            try {
                if (!std::filesystem::exists(java_path) || !std::filesystem::exists(server_dir)) {
                    return false;
                }
            } catch (const std::filesystem::filesystem_error&) {
                return false;
            }
            
            return true;
        }
    } config_;

    void load_config(json config_data);

    /* RCON методы
    void setup_rcon();
    void rcon_connection_worker();
    void check_server_ready_via_rcon();*/

    /* Внутренние потоки */
    void read_output();            // Чтение stdout сервера
    void monitor_process_exit();   // Ожидание смерти процесса

    /* Вспомогалки */
    static std::string get_last_error_message(DWORD error_code);

    /* Состояние */
    std::atomic<bool>      running_{false};
    std::atomic<bool>      ready_{false};
    std::atomic<ServerStatus> status_{ServerStatus::Stopped};
    std::atomic<bool>      rcon_enabled_{false};
    std::atomic<bool>      rcon_connecting_{false};

    /* IPC-хендлы */
    HANDLE stdinPipe_ {nullptr};
    HANDLE readPipe_  {nullptr};
    PROCESS_INFORMATION procInfo_{};

    /* Потоки */
    std::thread output_thread_;
    std::thread process_monitor_thread_;
    std::thread rcon_thread_;

    /* RCON 
    RCONClient rcon_;*/
};