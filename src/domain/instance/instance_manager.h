#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include <memory>
#include <chrono>

#include "process_manager.h"

// forward
class IRuntime;
class RuntimeRegistry;

enum class InstanceState {
    STOPPED,
    STARTING,
    RUNNING,
    STOPPING,
    CRASHED,
    UNHEALTHY
};

struct InstanceConfig {
    std::string id;
    std::string runtime_id;
    std::string path;
    int port = 25565;
};

struct Instance {
    InstanceConfig config;

    ProcessManager::Pid pid = 0;
    InstanceState state = InstanceState::STOPPED;

    const IRuntime* runtime = nullptr;

    std::chrono::steady_clock::time_point last_start{};
    std::chrono::steady_clock::time_point stop_requested_at{};

    int restart_count = 0;
};

class InstanceManager {
public:
    InstanceManager(RuntimeRegistry& rr, ProcessManager& pm);

    void register_instance(const InstanceConfig& config);

    bool start(const std::string& id);
    bool stop(const std::string& id);
    bool restart(const std::string& id);
    bool kill(const std::string& id);

    bool send_command(const std::string& id, const std::string& cmd);

    InstanceState get_state(const std::string& id) const;
    bool exists(const std::string& id) const;

private:
    void handle_stdout(const std::string& id, const std::string& line);
    void handle_exit(const std::string& id);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Instance> instances_;

    RuntimeRegistry& runtimeRegistry_;
    ProcessManager& processManager_;
};