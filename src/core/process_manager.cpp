#include "process_manager.h"
#include "../includes/logger.h"
#include "../utils/ipc/ipc_client.h"

#include <thread>
#include <vector>

// ─────────────────────────────────────────────
// CONFIG
// ─────────────────────────────────────────────
static constexpr int MAX_RESTARTS = 5;
static constexpr int RESTART_WINDOW_SEC = 30;
static constexpr int BASE_DELAY_MS = 1000;

// ─────────────────────────────────────────────
// CTOR / DTOR
// ─────────────────────────────────────────────
ProcessManager::ProcessManager() {

    mc_.name = "MC Worker";
    mc_.cmd  = "mshost.exe --mode=mc_worker";
    mc_.pipe_name = "\\\\.\\pipe\\mc_worker";

    web_.name = "WEB Worker";
    web_.cmd  = "mshost.exe --mode=web_worker";
    web_.pipe_name = "\\\\.\\pipe\\web_worker";
}

ProcessManager::~ProcessManager() {
    stop_watchdog();

    stop_process(mc_);
    stop_process(web_);
}

// ─────────────────────────────────────────────
// PUBLIC API
// ─────────────────────────────────────────────
bool ProcessManager::start_mc()  { return start_process(mc_); }
bool ProcessManager::start_web() { return start_process(web_); }

void ProcessManager::stop_mc()  { stop_process(mc_); }
void ProcessManager::stop_web() { stop_process(web_); }

// ─────────────────────────────────────────────
// START
// ─────────────────────────────────────────────
bool ProcessManager::start_process(ManagedProcess& proc) {

    if (proc.state == State::Running || proc.state == State::Starting) {
        LOG_WARNING(proc.name + " уже запущен", "CORE");
        return false;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);

    ZeroMemory(&proc.pi, sizeof(proc.pi));

    DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NEW_CONSOLE;

    BOOL ok = CreateProcessA(
        nullptr,
        const_cast<char*>(proc.cmd.c_str()),
        nullptr,
        nullptr,
        FALSE,
        flags,
        nullptr,
        nullptr,
        &si,
        &proc.pi
    );

    if (!ok) {
        LOG_ERR("CreateProcess failed: " + proc.cmd, "CORE");
        proc.state = State::Dead;
        return false;
    }

    // Job Object (убивает всё дерево)
    proc.job = CreateJobObject(nullptr, nullptr);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
    jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    SetInformationJobObject(
        proc.job,
        JobObjectExtendedLimitInformation,
        &jobInfo,
        sizeof(jobInfo)
    );

    AssignProcessToJobObject(proc.job, proc.pi.hProcess);

    proc.state = State::Running;
    proc.lastStart = std::chrono::steady_clock::now();

    LOG_INFO(proc.name + " запущен", "CORE");
    return true;
}

// ─────────────────────────────────────────────
// IS RUNNING
// ─────────────────────────────────────────────
bool ProcessManager::is_running(ManagedProcess& proc) {

    if (!proc.pi.hProcess) {
        return false;
        proc.state = State::Stopped;
    };

    DWORD code = 0;

    if (!GetExitCodeProcess(proc.pi.hProcess, &code)) {
        return false;
        proc.state = State::Stopped;
    }

    return code == STILL_ACTIVE;
}

// ─────────────────────────────────────────────
// Send Command (IPC)
// ─────────────────────────────────────────────

void ProcessManager::send_mc_command(const std::string& cmd) {
    if (mc_.state != State::Running && !is_running(mc_)) {
        LOG_WARNING("MC не запущен", "CORE");
        return;
    }

    IPCClient::send(mc_.pipe_name, "cmd " + cmd, 10);
}

// ─────────────────────────────────────────────
// GRACEFUL STOP (IPC)
// ─────────────────────────────────────────────
bool ProcessManager::try_graceful_stop(ManagedProcess& proc, DWORD timeoutMs) {

    if (!is_running(proc))
        return true;

    LOG_INFO("Пытаемся мягко остановить " + proc.name + " через IPC", "CORE");

    if (proc.pipe_name.empty()) {
        LOG_WARNING("pipe_name не задан для " + proc.name, "CORE");
        return false;
    }

    bool sent = IPCClient::send(proc.pipe_name, "stop", 10);

    if (!sent) {
        LOG_WARNING("IPC send failed: " + proc.name, "CORE");
        return false;
    }

    DWORD wait = WaitForSingleObject(proc.pi.hProcess, timeoutMs);

    if (wait == WAIT_OBJECT_0) {
        LOG_INFO(proc.name + " остановился через IPC", "CORE");
        return true;
    }

    return false;
}

// ─────────────────────────────────────────────
// FORCE KILL (через JobObject)
// ─────────────────────────────────────────────
void ProcessManager::force_kill(ManagedProcess& proc) {

    LOG_WARNING("Force kill: " + proc.name, "CORE");

    if (proc.job) {
        CloseHandle(proc.job); // 💥 убивает всё дерево
        proc.job = nullptr;
    }

    if (proc.pi.hProcess) {
        TerminateProcess(proc.pi.hProcess, 1);
        CloseHandle(proc.pi.hProcess);
    }

    if (proc.pi.hThread) {
        CloseHandle(proc.pi.hThread);
    }

    ZeroMemory(&proc.pi, sizeof(proc.pi));

    proc.state = State::Dead;
}

// ─────────────────────────────────────────────
// STOP
// ─────────────────────────────────────────────
void ProcessManager::stop_process(ManagedProcess& proc) {

    if (proc.state != State::Running)
        return;

    LOG_INFO("Stopping " + proc.name, "CORE");

    proc.state = State::Stopping;

    if (!try_graceful_stop(proc, 15000)) {
        LOG_WARNING(proc.name + " graceful failed", "CORE");
        force_kill(proc);
        return;
    }

    // cleanup
    if (proc.job) {
        CloseHandle(proc.job);
        proc.job = nullptr;
    }

    CloseHandle(proc.pi.hProcess);
    CloseHandle(proc.pi.hThread);

    ZeroMemory(&proc.pi, sizeof(proc.pi));

    proc.state = State::Stopped;

    LOG_INFO(proc.name + " stopped cleanly", "CORE");
}

// ─────────────────────────────────────────────
// WAIT ALL
// ─────────────────────────────────────────────
void ProcessManager::wait_all() {
    std::vector<HANDLE> handles;

    if (mc_.pi.hProcess)
        handles.push_back(mc_.pi.hProcess);

    if (web_.pi.hProcess)
        handles.push_back(web_.pi.hProcess);

    if (handles.empty()) return;

    WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()),
        handles.data(),
        TRUE,
        15000
    );
}

// ─────────────────────────────────────────────
// WATCHDOG
// ─────────────────────────────────────────────
void ProcessManager::start_watchdog() {

    if (watchdog_running_) return;

    watchdog_running_ = true;

    watchdog_ = std::thread([this]() {

        while (watchdog_running_) {

            check_and_restart(mc_);
            check_and_restart(web_);

            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    LOG_INFO("Watchdog запущен", "CORE");
}

void ProcessManager::stop_watchdog() {

    watchdog_running_ = false;

    if (watchdog_.joinable())
        watchdog_.join();

    LOG_INFO("Watchdog остановлен", "CORE");
}

// ─────────────────────────────────────────────
// RESTART
// ─────────────────────────────────────────────
void ProcessManager::check_and_restart(ManagedProcess& proc) {

    if (proc.state != State::Running)
        return;

    if (is_running(proc))
        return;

    auto now = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - proc.lastStart
    ).count();

    if (elapsed > RESTART_WINDOW_SEC)
        proc.restartCount = 0;

    proc.restartCount++;

    if (proc.restartCount > MAX_RESTARTS) {
        LOG_CRITICAL(proc.name + " отключён (too many restarts)", "CORE");
        proc.state = State::Dead;
        return;
    }

    int delay = BASE_DELAY_MS * proc.restartCount;

    LOG_WARNING(proc.name + " упал, рестарт через " +
                std::to_string(delay) + " ms", "CORE");

    std::this_thread::sleep_for(std::chrono::milliseconds(delay));

    start_process(proc);
}