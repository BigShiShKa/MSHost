/*
Компиляция:
g++ ./src/main.cpp ./src/MinecraftServerManager.cpp ./src/httpServer.cpp -o ./bin/mshost -lws2_32
*/

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
#include <codecvt>
#include <locale>
#else
#include <signal.h>
#include <iostream>
#include <unistd.h>
#endif

#include "./includes/minecraftservermanager.h"
#include "./includes/httpServer.h"
#include "./includes/logger.h"

using json = nlohmann::json;    

// ────────────────────────── Глобальные ──────────────────────────
std::atomic<bool> running(true);
const std::string Version = "0.4.0.134a";

static MinecraftServerManager* g_mc   = nullptr; // для сигнал‑хендлеров
static HttpServer*          g_http = nullptr;
static std::thread          g_webThread;   // поток веб‑сервера
static std::atomic<bool>    webRunning{false};

// ────────────────────────── shutdown() ──────────────────────────
void request_shutdown(const char* why) {
    if (!running.exchange(false)) return; // уже в процессе
    LOG_INFO(std::string("Получен сигнал завершения ( ") + why + " )", "SHUTDOWN");

    if (webRunning) {
        g_http->stop();
        if (g_webThread.joinable()) g_webThread.join();
        webRunning = false;
    }
    if (g_mc)   g_mc->stop();
    Logger::instance().finalize();

#ifdef _WIN32
    // Разблокируем ReadConsole у input‑потока
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    CancelIoEx(hIn, nullptr);
    CloseHandle(hIn);
#endif
}

// ────────────────────────── Ctrl‑C / SIGINT ─────────────────────
#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD sig)
{
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT) {
        request_shutdown("Win console");
        return TRUE;          // мы всё обработали, жить продолжаем
    }

    if (sig == CTRL_CLOSE_EVENT) {
        request_shutdown("Console close X");

        // принудительно завершаем поток main, чтобы успеть
        // дойти до atexit / flush‑ов, но до того, как ОС убьёт нас
        Logger::instance().finalize(); // на всякий случай «ручной» вызов
        ExitProcess(0);                // корректный выход
        return TRUE;                   // не дойдём, но чтобы компилятор молчал
    }
    return FALSE;
}
#endif

// ────────────────────────── CLI Поток ───────────────────────────
void handle_input(MinecraftServerManager& manager, HttpServer& http) {
    std::string command;
    while (running) {
#ifdef _WIN32
        std::wstring wcmd;
        if (!std::getline(std::wcin, wcmd)) { request_shutdown("stdin EOF"); break; }
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cv; 
        command = cv.to_bytes(wcmd);
#else
        if (!std::getline(std::cin, command)) { request_shutdown("stdin EOF"); break; }
#endif
        if (command.empty()) continue;

        // === команды ===
        if (command == "server-start") {
            LOG_INFO("Инициализация запуска сервера...", "INPUT");
            manager.start();
        } 
        else if (command == "server-stop" || command == "stop") {
            LOG_INFO("Получена команда остановки сервера", "INPUT");
            manager.stop();
        } 
        else if (command == "server-restart") {
            LOG_INFO("Получена команда перезапуска сервера...", "INPUT");
            manager.stop();
            manager.start();
        } 
        else if (command == "web-start") {
            if (!webRunning) {
                webRunning = true;
                g_webThread = std::thread(&HttpServer::run, &http);
                LOG_INFO("HTTPS сервер запущен", "INPUT");
            } else {
                LOG_WARNING("HTTPS уже работает", "INPUT");
            }

        } 
        else if (command == "web-stop") {
            if (webRunning) {
                http.stop();
                if (g_webThread.joinable()) g_webThread.join();
                webRunning = false;
                LOG_INFO("HTTPS сервер остановлен", "INPUT");
            } else {
                LOG_WARNING("HTTPS не запущен", "INPUT");
            } 
        }
        else if (command == "web-restart") {
            LOG_INFO("HTTPS сервер перезапускается...", "INPUT");
            if (webRunning) {
                http.stop();
                if (g_webThread.joinable()) g_webThread.join();
                webRunning = false;
                LOG_INFO("HTTPS сервер остановлен", "INPUT");
            } else {
                LOG_WARNING("HTTPS не запущен", "INPUT");
            } 

            if (!webRunning) {
                webRunning = true;
                g_webThread = std::thread(&HttpServer::run, &http);
                LOG_INFO("HTTPS сервер запущен", "INPUT");
            } else {
                LOG_WARNING("HTTPS уже работает", "INPUT");
            }
        }
        else if (command == "web-updatetokens") {
            http.load_tokens();
            LOG_INFO("Команда обновления токенов выполнена", "INPUT");
        } 
        else if (command == "server-status") {
            LOG_INFO("Запрос статуса сервера", "INPUT");
            std::wcout << L"Статус сервера: " << manager.get_status() << std::endl;
        } 
        else if (command == "exit") {
            LOG_INFO("Остановка программы и серверов...", "INPUT");
            request_shutdown("exit cmd");
            break;
        } 
        else if (command == "help") {
            std::wcout << L"\nСписок доступных команд:\n"
                       << L"\"server-start/stop\" : Останавливает запущенный Minecraft Server\n"
                       << L"\"server-restart\" : Перезапускает Minecraft Server\n"
                       << L"\"server-status\" : Выводит статус сервера\n"
                       << L"\"web-start\" : Запускает Web Server\n"
                       << L"\"web-stop\" : Останавливает запущенный Web Server\n"
                       << L"\"web-restart\" : Перезапускает Web Server\n"
                       << L"\"web-updatetokens\" : перечитывает файл токенов\n"
                       << L"\"exit\" : Останавливает ВСЕ и завершает программу\n"
                       << L"\"help\" : Выводит список команд\n"
                       << L"\"prank <игрок>\" : Наносит психоурон игроку)))\n"
                       << L"\"/команда\" : Отправляет в консоль Minecraft Server\n" << std::endl;
        } 
        else if (command.rfind("prank",0) == 0 && command.size() > 5) {
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
                LOG_INFO(">>> Пранк успешно нанес психо‑урон", "PRANK");
            } else {
                LOG_WARNING("Сервер не запущен. Пранк отменён.", "PRANK");
            }

        } else {
            if (command[0] == '/') {
                if (manager.is_running()) {
                    manager.send_command(command.substr(1));
                } else {
                    LOG_WARNING("Команда к неработающему серверу", "INPUT");
                }
            } else {
                LOG_ERR("Неизвестная команда: " + command, "INPUT");
            }
        }
    }
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
            std::cerr << "Неизвестный параметр: " << a << "\n";
            return 1;
        }
    }

#ifdef _WIN32
    // 1. Всегда включаем UTF‑8 в кодовую страницу — это не мешает.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // 2. Проверяем: stdout – это реальная консоль или pipe/pty?
    DWORD dummy;
    bool realConsole = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &dummy);

    if (realConsole) {
        // ── cmd / PowerShell / Windows Terminal (без VSCode) ──
        _setmode(_fileno(stdout),  _O_U16TEXT);
        _setmode(_fileno(stderr),  _O_U16TEXT);
        _setmode(_fileno(stdin),   _O_U16TEXT);
        std::locale::global(std::locale(""));      // системная (UTF‑16)
        std::wcout.imbue(std::locale());
        std::wcerr.imbue(std::locale());
        std::wcin .imbue(std::locale());
    } else {
        // ── VSCode Terminal / MSYS2 / SSH‑pty / Git‑bash ──
        _setmode(_fileno(stdout),  _O_U8TEXT);
        _setmode(_fileno(stderr),  _O_U8TEXT);
        _setmode(_fileno(stdin),   _O_U8TEXT);
        std::locale utf8(".UTF-8");
        std::locale::global(utf8);
        std::wcout.imbue(utf8);
        std::wcerr.imbue(utf8);
        std::wcin .imbue(utf8);
    }
#else
        std::signal(SIGINT,  PosixSigHandler);
        std::signal(SIGTERM, PosixSigHandler);
#endif

    try {
        std::wcout << L"Запуск логгера..." << std::endl;
        Logger::instance().init(true);
        LOG_INFO("===== Майнкрафт Хост v" + Version + " =====", "MAIN");

        LOG_INFO("Чтение конфигураций...", "MAIN");
        json config;
        
        try {
            std::ifstream config_file;
            config_file.open("config.json");
            
            if (!config_file.is_open()) {
                LOG_CRITICAL("Не удалось найти config.json!", "MAIN");
                return 1;
            }
            
            config = json::parse(config_file);
            config_file.close();
            
        } catch (const std::exception& e) {
            LOG_CRITICAL(std::string("Ошибка чтения config.json: ") + e.what(), "MAIN");
            return 1;
        }

        LOG_INFO("Инициализация серверов...", "MAIN");
        MinecraftServerManager mcserver(config);

        HttpServer http(
            mcserver, 
            config["web"]["port"].get<std::int16_t>(),
            running,
            config["web"]["tokens_file"].get<std::string>(),
            config["web"]["logs_path"].get<std::string>(),
            config["web"]["modpack_path"].get<std::string>(),
            config["web"]["web_root"].get<std::string>(),
            config["web"]["upload_limit"].get<std::int16_t>()
        );

        g_mc   = &mcserver;
        g_http = &http;
        LOG_INFO("Успешно!", "MAIN");

        std::wcout << L"\nСписок доступных команд:\n"
            << L"\"server-start/stop\" : Останавливает запущенный Minecraft Server\n"
            << L"\"server-restart\" : Перезапускает Minecraft Server\n"
            << L"\"server-status\" : Выводит статус сервера\n"
            << L"\"web-start\" : Запускает Web Server\n"
            << L"\"web-stop\" : Останавливает запущенный Web Server\n"
            << L"\"web-restart\" : Перезапускает Web Server\n"
            << L"\"web-updatetokens\" : перечитывает файл токенов\n"
            << L"\"exit\" : Останавливает ВСЕ и завершает программу\n"
            << L"\"help\" : Выводит список команд\n"
            << L"\"prank <игрок>\" : Наносит психоурон игроку)))\n"
            << L"\"/команда\" : Отправляет в консоль Minecraft Server\n"
            << std::endl;

        if (flagWeb && !webRunning) {
            webRunning = true;
            g_webThread = std::thread(&HttpServer::run, &http);
            LOG_INFO("HTTP запущен по флагу", "MAIN");
        }
        if (flagMc) {
            mcserver.start();
        }
        LOG_INFO("Запуск потоков...", "MAIN");
        std::thread input_thread(handle_input, std::ref(mcserver), std::ref(http));
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
