#pragma once

#include "minecraftservermanager.h"
#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <chrono>

struct WebConfig {
    int port                 = 8080;
    std::string tokens_file  = "tokens";
    std::string logs_path;
    std::string modpack_path;
    std::string web_root     = "./site";
    int upload_limit_mb      = 7;
    int max_log_lines        = 500;
    int rate_limit_ms        = 1000;
    int thread_pool_size     = 4;

    // Информация о сервере для /api/status
    std::string server_ip      = "127.0.0.1";
    int         server_port    = 25565;
    std::string server_version = "Unknown";
};

class HttpServer {
public:
    HttpServer(MinecraftServerManager& manager,
               std::atomic<bool>& running,
               const WebConfig& config);

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    void run();
    void stop();
    void load_tokens();

private:
    std::atomic<bool>& running_;
    MinecraftServerManager& manager_;
    WebConfig config_;
    httplib::Server svr;

    nlohmann::json get_status_json();

    std::unordered_set<std::string> tokens_;
    std::mutex tokens_mx_;

    // Rate limiter
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> rate_map_;
    std::mutex rate_mx_;
    void cleanup_rate_map();

    bool check_token(const std::string&);

    void setup_routes();
};
