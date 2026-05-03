#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
//#include "../../includes/json.hpp" - потенциально json команды

class IPCServer {
public:
    explicit IPCServer(std::string pipe_name);

    void start(std::function<void(const std::string&)> handler);
    void stop();
    
private:
    void run(std::function<void(const std::string&)> handler);

private:
    std::string pipe_name_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};