#pragma once

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

enum class State {
    Starting,
    Running,
    Stopping,
    Stopped,
    Dead
};

struct ManagedProcess {
    PROCESS_INFORMATION pi{};
    HANDLE job = nullptr; // ключ к стабильности

    std::string name;
    std::string cmd;
    std::string pipe_name;

    State state = State::Stopped;

    int restartCount = 0;

    std::chrono::steady_clock::time_point lastStart{};
};

class ProcessManager {
public:
    ProcessManager();
    ~ProcessManager();

    bool start_mc();
    bool start_web();

    void send_mc_command(const std::string& cmd);

    void stop_mc();
    void stop_web();
    void wait_all();

    void start_watchdog();
    void stop_watchdog();

private:
    bool start_process(ManagedProcess& proc);
    void stop_process(ManagedProcess& proc);

    bool is_running(ManagedProcess& proc);

    bool try_graceful_stop(ManagedProcess& proc, DWORD timeoutMs);
    void force_kill(ManagedProcess& proc);

    void cleanup_process(ManagedProcess& proc);

    void check_and_restart(ManagedProcess& proc);

private:
    ManagedProcess mc_;
    ManagedProcess web_;

    std::thread watchdog_;
    std::atomic<bool> watchdog_running_{false};
};