#include <thread>
#include <atomic>
#include <string>
#include <algorithm>
#include <csignal>
#include <fstream>
#include "./includes/json.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <iostream>
#include <unistd.h>
#endif

#include "./includes/minecraftservermanager.h"
#include "./includes/caddy_manager.h"
#include "./includes/httpServer.h"
#include "./includes/logger.h"

using json = nlohmann::json;

// ────────────────────────── Глобальные ──────────────────────────
std::atomic<bool> running(true);
const std::string Version = "0.5.0";

static MinecraftServerManager* g_mc    = nullptr;
static HttpServer*             g_http  = nullptr;
static CaddyManager*           g_caddy = nullptr;
static std::thread             g_webThread;
static std::atomic<bool>       webRunning{false};

// ────────────────────────── Консольный вывод ─────────────────────
// Вместо std::wcout используем WriteConsoleW напрямую,
// чтобы избежать краша CRT при _O_U16TEXT + imbue(locale(""))
#ifdef _WIN32
static void wprint(const std::wstring& text) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteConsoleW(hOut, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
}
static void wprintln(const std::wstring& text) {
    wprint(text + L"\n");
}
#else
static void wprintln(const std::wstring& text) {
    std::wcout << text << std::endl;
}
#endif

// ────────────────────────── Help text ───────────────────────────
static const std::wstring HELP_TEXT =
    L"\nСписок доступных команд:\n"
    L"  server-start     : Запускает Minecraft Server\n"
    L"  server-stop      : Останавливает Minecraft Server\n"
    L"  server-restart   : Перезапускает Minecraft Server\n"
    L"  server-status    : Выводит статус сервера\n"
    L"  web-start        : Запускает Web Server\n"
    L"  web-stop         : Останавливает Web Server\n"
    L"  web-restart      : Перезапускает Web Server\n"
    L"  web-updatecreds  : Перечитывает файл учётных данных\n"
    L"  caddy-start      : Запускает Caddy (HTTPS reverse proxy)\n"
    L"  caddy-stop       : Останавливает Caddy\n"
    L"  caddy-restart    : Перезапускает Caddy\n"
    L"  caddy-status     : Выводит статус Caddy\n"
    L"  exit             : Останавливает ВСЕ и завершает программу\n"
    L"  help             : Выводит список команд\n"
    L"  prank <игрок>    : Наносит психоурон игроку)))\n"
    L"  /команда         : Отправляет в консоль Minecraft Server\n";

// ────────────────────────── shutdown() ──────────────────────────
void request_shutdown(const char* why) {
    if (!running.exchange(false)) return;
    LOG_INFO(std::string("Получен сигнал завершения ( ") + why + " )", "SHUTDOWN");

    if (g_caddy) g_caddy->stop();
    if (webRunning) {
        g_http->stop();
        if (g_webThread.joinable()) g_webThread.join();
        webRunning = false;
    }
    if (g_mc) g_mc->stop();
    Logger::instance().finalize();

#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    CancelIoEx(hIn, nullptr);
    CloseHandle(hIn);
#endif
}

// ────────────────────────── Ctrl-C / SIGINT ─────────────────────
#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD sig)
{
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT) {
        request_shutdown("Win console");
        return TRUE;
    }
    if (sig == CTRL_CLOSE_EVENT) {
        request_shutdown("Console close X");
        Logger::instance().finalize();
        ExitProcess(0);
        return TRUE;
    }
    return FALSE;
}
#endif

// ────────────────── Конвертация wstring → string (Win32 API) ────
#ifdef _WIN32
static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                  static_cast<int>(wstr.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string result(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                        static_cast<int>(wstr.size()),
                        result.data(), sz, nullptr, nullptr);
    return result;
}
#endif

// ────────────── Вспомогательные: start/stop web ─────────────────
static WebConfig load_web_config(const json& config);

static void start_web(HttpServer& http) {
    if (webRunning) {
        LOG_WARNING("HTTP уже работает", "INPUT");
        return;
    }
    webRunning = true;
    g_webThread = std::thread(&HttpServer::run, &http);
    LOG_INFO("HTTP сервер запущен", "INPUT");
}

static void stop_web(HttpServer& http) {
    if (!webRunning) {
        LOG_WARNING("HTTP не запущен", "INPUT");
        return;
    }
    http.stop();
    if (g_webThread.joinable()) g_webThread.join();
    webRunning = false;
    LOG_INFO("HTTP сервер остановлен", "INPUT");
}

// ────────────────────────── CLI Поток ───────────────────────────
void handle_input(MinecraftServerManager& manager, HttpServer& http, CaddyManager& caddy) {
    std::string command;
    while (running) {
#ifdef _WIN32
        std::wstring wcmd;
        if (!std::getline(std::wcin, wcmd)) { request_shutdown("stdin EOF"); break; }
        command = wide_to_utf8(wcmd);
#else
        if (!std::getline(std::cin, command)) { request_shutdown("stdin EOF"); break; }
#endif
        if (command.empty()) continue;

        if (command == "server-start") {
            LOG_INFO("Инициализация запуска сервера...", "INPUT");
            try {
                std::ifstream cf("config.json");
                if (cf.is_open()) manager.reload_config(json::parse(cf));
            } catch (const std::exception& e) {
                LOG_ERR(std::string("Ошибка чтения конфига: ") + e.what(), "INPUT");
            }
            manager.start();
        }
        else if (command == "server-stop" || command == "stop") {
            LOG_INFO("Получена команда остановки сервера", "INPUT");
            manager.stop();
        }
        else if (command == "server-restart") {
            LOG_INFO("Получена команда перезапуска сервера...", "INPUT");
            manager.stop();
            try {
                std::ifstream cf("config.json");
                if (cf.is_open()) manager.reload_config(json::parse(cf));
            } catch (const std::exception& e) {
                LOG_ERR(std::string("Ошибка чтения конфига: ") + e.what(), "INPUT");
            }
            manager.start();
        }
        else if (command == "web-start") {
            if (caddy.is_enabled() && !caddy.is_running()) caddy.start();
            start_web(http);
        }
        else if (command == "web-stop") {
            caddy.stop();
            stop_web(http);
        }
        else if (command == "web-restart") {
            LOG_INFO("HTTP сервер перезапускается...", "INPUT");
            caddy.stop();
            stop_web(http);
            try {
                std::ifstream cf("config.json");
                if (cf.is_open()) {
                    json new_config = json::parse(cf);
                    http.update_config(load_web_config(new_config));
                    caddy.reload_config(new_config);
                    LOG_INFO("Конфиг перечитан", "INPUT");
                }
            } catch (const std::exception& e) {
                LOG_ERR(std::string("Ошибка перечитывания конфига: ") + e.what(), "INPUT");
            }
            start_web(http);
            if (caddy.is_enabled()) caddy.start();
        }
        else if (command == "web-updatecreds") {
            http.load_credentials();
            LOG_INFO("Учётные данные обновлены", "INPUT");
        }
        else if (command == "caddy-start") {
            try {
                std::ifstream cf("config.json");
                if (cf.is_open()) caddy.reload_config(json::parse(cf));
            } catch (const std::exception& e) {
                LOG_ERR(std::string("Ошибка чтения конфига: ") + e.what(), "INPUT");
            }
            caddy.start();
        }
        else if (command == "caddy-stop") {
            caddy.stop();
        }
        else if (command == "caddy-restart") {
            try {
                std::ifstream cf("config.json");
                if (cf.is_open()) caddy.reload_config(json::parse(cf));
            } catch (const std::exception& e) {
                LOG_ERR(std::string("Ошибка чтения конфига: ") + e.what(), "INPUT");
            }
            caddy.restart();
        }
        else if (command == "caddy-status") {
            wprintln(L"Статус Caddy: " + std::wstring(caddy_status_text_wide[static_cast<int>(caddy.get_status())]));
        }
        else if (command == "server-status") {
            LOG_INFO("Запрос статуса сервера", "INPUT");
            wprintln(L"Статус сервера: " + std::wstring(status_text_wide[static_cast<int>(manager.get_status())]));
        }
        else if (command == "exit") {
            LOG_INFO("Остановка программы и серверов...", "INPUT");
            request_shutdown("exit cmd");
            break;
        }
        else if (command == "help") {
            wprintln(HELP_TEXT);
        }
        else if (command.rfind("prank", 0) == 0 && command.size() > 5) {
            if (manager.is_running()) {
                std::string player = command.substr(5);
                player.erase(0, player.find_first_not_of(" \t"));
                player.erase(player.find_last_not_of(" \t") + 1);
                LOG_INFO("Выполнение пранка для игрока: " + player, "PRANK");
                manager.send_command("weather thunder");
                manager.send_command("title " + player + " times 2s 5s 2s");
                manager.send_command("title " + player + " title {\"text\":\"...\",\"color\":\"dark_red\",\"bold\":true}");
                std::this_thread::sleep_for(std::chrono::seconds(9));
                manager.send_command("execute at " + player + " run playsound midnightlurker:lurkerchase master " + player + " ~ ~ ~ 1 1 1");
                manager.send_command("title " + player + " title [{\"text\":\"X\",\"obfuscated\":true,\"color\":\"red\",\"bold\":true},{\"text\":\" Run! \",\"color\":\"red\",\"bold\":true},{\"text\":\"Z\",\"obfuscated\":true,\"color\":\"red\",\"bold\":true}]");
                LOG_INFO(">>> Пранк успешно нанес психо-урон", "PRANK");
            } else {
                LOG_WARNING("Сервер не запущен. Пранк отменён.", "PRANK");
            }
        }
        else if (command[0] == '/') {
            if (manager.is_running()) {
                manager.send_command(command.substr(1));
            } else {
                LOG_WARNING("Команда к неработающему серверу", "INPUT");
            }
        }
        else {
            LOG_ERR("Неизвестная команда: " + command, "INPUT");
        }
    }
}

// ────────────────────────── Загрузка WebConfig из JSON ───────────
static WebConfig load_web_config(const json& config) {
    WebConfig wc;
    const auto& web = config["web"];

    wc.port            = web.value("port", 8080);
    wc.tokens_file     = web.value("tokens_file", "tokens");
    wc.logs_path       = web.value("logs_path", "server.log");
    wc.modpack_path    = web.value("modpack_path", "");
    wc.web_root        = web.value("web_root", "./site");
    wc.upload_limit_mb = web.value("upload_limit", 7);
    wc.max_log_lines   = web.value("max_log_lines", 500);
    wc.rate_limit_ms   = web.value("rate_limit_ms", 1000);
    wc.thread_pool_size = web.value("thread_pool_size", 4);

    // Информация о MC-сервере для /api/status
    wc.server_ip      = web.value("server_ip", "127.0.0.1");
    wc.server_port    = web.value("server_port", 25565);
    wc.server_version = web.value("server_version", "Unknown");

    return wc;
}

// ────────────────────────── main() ─────────────────────────────
int main(int argc, char* argv[])
{
    bool flagMc  = false;
    bool flagWeb = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--all")      { flagMc = flagWeb = true; }
        else if (a == "--mc-only")  { flagMc = true;  }
        else if (a == "--web-only") { flagWeb = true; }
        else {
            // До настройки консоли — используем MessageBoxA для ошибок
#ifdef _WIN32
            MessageBoxA(nullptr, ("Unknown argument: " + a).c_str(), "MSHost", MB_OK);
#endif
            return 1;
        }
    }

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // Stdin в U16TEXT для чтения кириллицы через std::wcin
    _setmode(_fileno(stdin), _O_U16TEXT);
    // stdout/stderr — НЕ трогаем _setmode, весь вывод через WriteConsoleW / Logger
#else
    std::signal(SIGINT,  PosixSigHandler);
    std::signal(SIGTERM, PosixSigHandler);
#endif

    try {
        wprintln(L"Запуск логгера...");
        Logger::instance().init(true);
        LOG_INFO("===== Майнкрафт Хост v" + Version + " =====", "MAIN");

        LOG_INFO("Чтение конфигураций...", "MAIN");
        json config;

        try {
            std::ifstream config_file("config.json");
            if (!config_file.is_open()) {
                LOG_CRITICAL("Не удалось найти config.json!", "MAIN");
                return 1;
            }
            config = json::parse(config_file);
        } catch (const std::exception& e) {
            LOG_CRITICAL(std::string("Ошибка чтения config.json: ") + e.what(), "MAIN");
            return 1;
        }

        LOG_INFO("Инициализация серверов...", "MAIN");
        MinecraftServerManager mcserver(config);
        CaddyManager caddy(config);

        WebConfig webConfig = load_web_config(config);
        HttpServer http(mcserver, running, webConfig);

        g_mc    = &mcserver;
        g_http  = &http;
        g_caddy = &caddy;
        http.on_exit = []() { request_shutdown("api/exit"); };
        LOG_INFO("Успешно!", "MAIN");

        wprintln(HELP_TEXT);

        // Автозапуск Caddy если включён в конфиге
        if (caddy.is_enabled()) {
            caddy.start();
        }

        if (flagWeb && !webRunning) {
            start_web(http);
            LOG_INFO("HTTP запущен по флагу", "MAIN");
        }
        if (flagMc) {
            mcserver.start();
        }

        LOG_INFO("Запуск потоков...", "MAIN");
        std::thread input_thread(handle_input, std::ref(mcserver), std::ref(http), std::ref(caddy));
        LOG_INFO("Успешно!", "MAIN");

        input_thread.join();
        if (g_webThread.joinable()) g_webThread.join();

        LOG_INFO("Программа завершена", "MAIN");
        Logger::instance().finalize();
        return 0;
    } catch (const std::exception& e) {
        LOG_CRITICAL(std::string("Fatal: ") + e.what(), "MAIN");
        return 1;
    }
}
