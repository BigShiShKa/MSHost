#include "web_worker.h"

#include "../includes/web_sv.h"
#include "../includes/caddy_manager.h"
#include "../includes/minecraft_sv_manager.h"
#include "../includes/logger.h"
#include "../includes/json.hpp"
#include "../utils/console.h"
#include "../utils/ipc/ipc_server.h"

#include <fstream>
#include <thread>
#include <atomic>

using json = nlohmann::json;

// ─────────────────────────────
// load web config
// ─────────────────────────────
static WebConfig load_web_config(const json& config) {
    WebConfig wc;

    const auto& web = config["web"];

    wc.port             = web.value("port", 8080);
    wc.tokens_file      = web.value("tokens_file", "tokens");
    wc.logs_path        = web.value("logs_path", "server.log");
    wc.modpack_path     = web.value("modpack_path", "");
    wc.web_root         = web.value("web_root", "./site");
    wc.upload_limit_mb  = web.value("upload_limit", 7);
    wc.max_log_lines    = web.value("max_log_lines", 500);
    wc.rate_limit_ms    = web.value("rate_limit_ms", 1000);
    wc.thread_pool_size = web.value("thread_pool_size", 4);

    wc.server_ip      = web.value("server_ip", "127.0.0.1");
    wc.server_port    = web.value("server_port", 25565);
    wc.server_version = web.value("server_version", "Unknown");

    return wc;
}

// ─────────────────────────────
// main
// ─────────────────────────────
int run_web_worker(int argc, char* argv[]) {
    setup_console();

    Logger::instance().init(true, "web.log");

    LOG_INFO("WEB Worker запущен", "WEB_WORKER");

    // ───────── CONFIG ─────────
    json config;

    try {
        std::ifstream f("config.json");
        if (!f.is_open()) {
            LOG_CRITICAL("config.json не найден", "WEB_WORKER");
            return 1;
        }
        config = json::parse(f);
    } catch (const std::exception& e) {
        LOG_CRITICAL(std::string("Ошибка конфига: ") + e.what(), "WEB_WORKER");
        return 1;
    }

    // ───────── SERVICES ─────────
    MinecraftServerManager mc(config); // нужен для API
    CaddyManager caddy(config);

    WebConfig webConfig = load_web_config(config);

    std::atomic<ServerStatus> web_status{ServerStatus::Starting};

    IPCServer ipc("\\\\.\\pipe\\web_worker");

    ipc.start([&](const std::string& msg) {

        if (msg == "stop") {
            LOG_INFO("IPC: stop", "WEB_WORKER");
            web_status = ServerStatus::Stopping;
        }

        else if (msg == "restart") {
            LOG_INFO("IPC: restart", "WEB_WORKER");
            web_status = ServerStatus::Stopping;
        }

    });

    HttpServer http(mc, web_status, webConfig);

    http.on_exit = [&]() {
        LOG_INFO("Получен сигнал остановки через API", "WEB_WORKER");
        web_status = ServerStatus::Stopping;
    };

    // ───────── START ─────────
    if (caddy.is_enabled()) {
        caddy.start();
    }

    std::thread http_thread([&]() {
        http.run();
    });

    LOG_INFO("WEB сервер запущен", "WEB_WORKER");
    web_status = ServerStatus::Running;

    // ───────── LOOP ─────────
    while (web_status.load() != ServerStatus::Stopping) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ───────── STOP ─────────
    http.stop();

    ipc.stop();

    if (http_thread.joinable())
        http_thread.join();

    caddy.stop();

    LOG_INFO("WEB Worker завершён", "WEB_WORKER");

    Logger::instance().finalize();
    return 0;
}