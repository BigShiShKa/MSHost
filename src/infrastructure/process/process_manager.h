#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <functional>
#include <atomic>

class ProcessManager {
public:
    using Pid = uint32_t;

    struct Config {
        std::string working_dir;
        std::string command;

        std::function<void(const std::string&)> on_stdout;
        std::function<void()> on_exit;
    };

public:
    ProcessManager();
    ~ProcessManager();

    Pid start(const Config& cfg);

    void write_stdin(Pid pid, const std::string& data);
    void stop(Pid pid);

    bool is_running(Pid pid);

private:
    struct Process {
        PROCESS_INFORMATION pi{};

        HANDLE job = nullptr;
        HANDLE stdin_write = nullptr;
        HANDLE stdout_read = nullptr;

        std::thread stdout_thread;
        std::thread exit_thread;

        std::function<void(const std::string&)> on_stdout;
        std::function<void()> on_exit;

        // запрет копирования
        Process(const Process&) = delete;
        Process& operator=(const Process&) = delete;

        // разрешаем перемещение
        Process(Process&&) noexcept = default;
        Process& operator=(Process&&) noexcept = default;

        Process() = default;
    };

private:
    void start_stdout_thread(Pid pid);
    void start_exit_thread(Pid pid);
    void cleanup(Pid pid);

private:
    std::unordered_map<Pid, Process> processes_;
    std::mutex mutex_;

    std::atomic<Pid> next_pid_{1};
};