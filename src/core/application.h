#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <string>
#include <memory>

#include "../includes/json.hpp"
#include "process_manager.h"

struct Command {
    std::string name;
    std::string description;
    std::function<void(const std::string&)> handler;
};

class Application {
public:
    Application(int argc, char* argv[]);
    ~Application();

    int run();
    void shutdown(const std::string& reason);

private:
    // lifecycle
    void init();
    void start();
    void parse_args(int argc, char* argv[]);

    // config
    void load_config();

    // CLI
    void start_cli();
    void cli_loop();
    void handle_command(const std::string& input);
    void register_commands();

private:
    // состояние
    std::atomic<bool> running_{true};

    // режим запуска
    enum class Mode {
        ALL,
        MC_ONLY,
        WEB_ONLY,
        NONE
    } mode_ = Mode::NONE;

    // управление процессами
    std::unique_ptr<ProcessManager> process_;

    // config
    nlohmann::json config_;

    // CLI
    std::thread cli_thread_;

    // команды
    using CommandHandler = std::function<void(const std::string&)>;
    std::vector<Command> commands_;
};