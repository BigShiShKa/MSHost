#include "application.h"
#include "../includes/logger.h"
#include "process_manager.h"
#include "../utils/console.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

// ─────────────────────────────────────────────
// CTRL + C Handler
// ─────────────────────────────────────────────

#ifdef _WIN32
static Application* g_app_instance = nullptr;

BOOL WINAPI CtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        if (g_app_instance) {
            g_app_instance->shutdown("CTRL+C");
        }
        return TRUE;
    }
    return FALSE;
}
#endif

// ─────────────────────────────────────────────
// Args parse
// ─────────────────────────────────────────────
void Application::parse_args(int argc, char* argv[]) {
    mode_ = Mode::NONE; // default

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--mc-only") {
            mode_ = Mode::MC_ONLY;
        }
        else if (arg == "--web-only") {
            mode_ = Mode::WEB_ONLY;
        }
        else if (arg == "--all") {
            mode_ = Mode::ALL;
        }
    }
}

// ─────────────────────────────────────────────
// helpers
// ─────────────────────────────────────────────
static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

// ─────────────────────────────────────────────
// ctor / dtor
// ─────────────────────────────────────────────
Application::Application(int argc, char* argv[]) {
    parse_args(argc, argv);
}

Application::~Application() = default;

// ─────────────────────────────────────────────
// run()
// ─────────────────────────────────────────────
int Application::run() {

    init();
    start();

    // Ждём пока running_ не станет false
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // CLI поток нужно аккуратно завершить
    if (cli_thread_.joinable()) {
        cli_thread_.join();
    };

    shutdown("normal exit");

    LOG_INFO("Core завершён", "CORE");
    return 0;
}

// ─────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────
void Application::init() {
    Logger::instance().init(
        true,
        "mc.log",
        "web.log"
    );

    LOG_INFO("Инициализация CORE...", "CORE");

    #ifdef _WIN32
        g_app_instance = this;
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
    #endif

    load_config();

    process_ = std::make_unique<ProcessManager>();

    register_commands();
}

// ─────────────────────────────────────────────
// start()
// ─────────────────────────────────────────────
void Application::start() {
    start_cli();

    // автозапуск по флагам
    if (mode_ == Mode::ALL || mode_ == Mode::MC_ONLY) {
        process_->start_mc();
    }

    if (mode_ == Mode::ALL || mode_ == Mode::WEB_ONLY) {
        process_->start_web();
    }
}

// ─────────────────────────────────────────────
// shutdown()
// ─────────────────────────────────────────────
void Application::shutdown(const std::string& reason) {
    if (!running_.exchange(false)) return;

    LOG_INFO("Shutdown: " + reason, "CORE");

    if (process_) {
        process_->wait_all();
    }

    Logger::instance().finalize();
}

// ─────────────────────────────────────────────
// CLI
// ─────────────────────────────────────────────
void Application::start_cli() {
    cli_thread_ = std::thread(&Application::cli_loop, this);
}

void Application::cli_loop() {
    while (running_) {

        auto input = read_line();

        if (input == "__EOF__") {
            shutdown("stdin EOF");
            break;
        }

        input = trim(input);
        if (input.empty()) continue;

        if (!running_) break;

        handle_command(input);
    }
}

// ─────────────────────────────────────────────
// Commands
// ─────────────────────────────────────────────
void Application::register_commands() {

    commands_ = {

        {
            "server-start",
            "Запуск Minecraft worker",
            [this](const std::string&) {
                process_->start_mc();
            }
        },

        {
            "server-stop",
            "Остановка Minecraft worker",
            [this](const std::string&) {
                process_->stop_mc();
            }
        },

        {
            "web-start",
            "Запуск Web worker",
            [this](const std::string&) {
                process_->start_web();
            }
        },

        {
            "web-stop",
            "Остановка Web worker",
            [this](const std::string&) {
                process_->stop_web();
            }
        },

        {
            "exit",
            "Выход",
            [this](const std::string&) {
                shutdown("cli exit");
            }
        },

        {
            "help",
            "Список команд",
            [this](const std::string&) {
                std::cout << "\nКоманды:\n";
                for (const auto& c : commands_) {
                    std::cout << "  "
                            << std::left << std::setw(15) << c.name
                            << c.description << "\n";
                }
                std::cout << "  /<cmd>        Отправить команду в Minecraft\n";
            }
        }
    };
}

// ─────────────────────────────────────────────
// handle_command()
// ─────────────────────────────────────────────
void Application::handle_command(const std::string& input) {

    // Перехват mc команд
    if (!input.empty() && input[0] == '/') {
        std::string mc_cmd = input.substr(1); // убираем '/'

        if (process_) {
            process_->send_mc_command(mc_cmd);
        }

        return;
    }

    auto space = input.find(' ');

    std::string cmd  = (space == std::string::npos) ? input : input.substr(0, space);
    std::string args = (space == std::string::npos) ? ""    : input.substr(space + 1);

    auto it = std::find_if(commands_.begin(), commands_.end(),
        [&](const Command& c) { return c.name == cmd; });

    if (it != commands_.end()) {
        it->handler(args);
        return;
    }

    LOG_ERR("Неизвестная команда: " + input, "CORE");
}

// ─────────────────────────────────────────────
// CONFIG
// ─────────────────────────────────────────────
void Application::load_config() {
    LOG_INFO("Загрузка конфига...", "CORE");
    std::ifstream f("config.json");

    if (!f.is_open()) {
        throw std::runtime_error("config.json не найден");
    }

    LOG_INFO("Конфиг загружен!", "CORE");

    config_ = nlohmann::json::parse(f);
}