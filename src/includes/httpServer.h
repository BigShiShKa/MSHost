#pragma once

#include "minecraftservermanager.h"
#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <atomic>

class HttpServer {
public:
    HttpServer(MinecraftServerManager& manager, 
              int port, 
              std::atomic<bool>& running, 
              const std::string& tokens_file,
              const std::string& logs_path,
              const std::string& modpack_path,
              const std::string& web_root,
              int upload_limit);

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
    int port_;
    httplib::Server svr;
    nlohmann::json get_status_json();
    int upload_limit_;

    std::unordered_set<std::string> tokens_;
    std::string        tokens_file_;
    std::mutex         tokens_mx_;

    std::string logs_path_;
    std::string modpack_path_;
    std::string web_root_;

    bool check_token(const std::string&);
};
