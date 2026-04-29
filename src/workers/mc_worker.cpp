#include "mc_worker.h"

#include "../includes/minecraft_sv_manager.h"
#include "../includes/logger.h"
#include "../includes/json.hpp"
#include "../utils/console.h"
#include "../utils/ipc/ipc_server.h"

#include <fstream>
#include <thread>
#include <atomic>

using json = nlohmann::json;

int run_mc_worker(int argc, char* argv[]) {
    setup_console();

    Logger::instance().init(true, "mc.log");

    LOG_INFO("MC Worker запущен", "MC_WORKER");

    // ───────── CONFIG ─────────
    json config;

    try {
        std::ifstream f("config.json");
        if (!f.is_open()) {
            LOG_CRITICAL("config.json не найден", "MC_WORKER");
            return 1;
        }
        config = json::parse(f);
    } catch (const std::exception& e) {
        LOG_CRITICAL(std::string("Ошибка конфига: ") + e.what(), "MC_WORKER");
        return 1;
    }

    // ───────── MC ─────────
    MinecraftServerManager mc(config);

    mc.start();

    std::atomic<bool> running{true};

    IPCServer ipc("\\\\.\\pipe\\mc_worker");

    ipc.start([&](const std::string& msg) {

        if (msg == "stop") {
            LOG_INFO("IPC: stop", "MC_WORKER");
            mc.stop();
            running = false;
        }
        else if (msg.rfind("cmd:", 0) == 0) {
            std::string cmd = msg.substr(4);
            mc.send_command(cmd);
        }

    });

    LOG_INFO("MC сервер запущен", "MC_WORKER");

    // ───────── LOOP ─────────

    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (mc.get_status() == ServerStatus::Stopped || mc.get_status() == ServerStatus::Dead) {
            LOG_WARNING("MC сервер остановился", "MC_WORKER");
            break;
        }
    }

    mc.stop();

    LOG_INFO("MC Worker завершён", "MC_WORKER");

    Logger::instance().finalize();
    return 0;
}