#include "process_manager.h"

#include <vector>
#include <stdexcept>

// ─────────────────────────────
// ctor / dtor
// ─────────────────────────────

ProcessManager::ProcessManager() {}

ProcessManager::~ProcessManager() {
    std::vector<Pid> to_stop;

    {
        std::lock_guard lock(mutex_);
        for (auto& [pid, _] : processes_) {
            to_stop.push_back(pid);
        }
    }

    // убиваем вне mutex
    for (Pid pid : to_stop) {
        stop(pid);
    }
}

// ─────────────────────────────
// start
// ─────────────────────────────

ProcessManager::Pid ProcessManager::start(const Config& cfg) {
    Process proc{};

    proc.on_stdout = cfg.on_stdout;
    proc.on_exit   = cfg.on_exit;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
        throw std::runtime_error("CreatePipe stdout failed");

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        throw std::runtime_error("CreatePipe stdin failed");
    }

    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdOutput = stdout_write;
    si.hStdError  = stdout_write;
    si.hStdInput  = stdin_read;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};

    std::vector<char> cmd(cfg.command.begin(), cfg.command.end());
    cmd.push_back('\0');

    std::vector<char> dir(cfg.working_dir.begin(), cfg.working_dir.end());
    dir.push_back('\0');

    if (!CreateProcessA(
            nullptr,
            cmd.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            dir.data(),
            &si,
            &pi))
    {
        DWORD err = GetLastError();

        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);

        throw std::runtime_error("CreateProcess failed: " + std::to_string(err));
    }

    // parent не использует
    CloseHandle(stdout_write);
    CloseHandle(stdin_read);

    proc.pi = pi;
    proc.stdout_read = stdout_read;
    proc.stdin_write = stdin_write;

    // JobObject
    proc.job = CreateJobObject(nullptr, nullptr);
    if (proc.job) {
        AssignProcessToJobObject(proc.job, pi.hProcess);
    }

    Pid pid;

    {
        std::lock_guard lock(mutex_);
        pid = next_pid_++;
        processes_.emplace(pid, std::move(proc));
    }

    start_stdout_thread(pid);
    start_exit_thread(pid);

    return pid;
}

// ─────────────────────────────
// stdout thread
// ─────────────────────────────

void ProcessManager::start_stdout_thread(Pid pid) {
    std::lock_guard lock(mutex_);

    auto it = processes_.find(pid);
    if (it == processes_.end()) return;

    it->second.stdout_thread = std::thread([this, pid]() {
        char buffer[1024];
        DWORD bytesRead = 0;

        std::string lineBuffer;

        while (true) {
            HANDLE pipe = nullptr;
            HANDLE procHandle = nullptr;
            std::function<void(const std::string&)> cb;

            {
                std::lock_guard lock(mutex_);
                auto it = processes_.find(pid);
                if (it == processes_.end()) return;

                pipe = it->second.stdout_read;
                procHandle = it->second.pi.hProcess;
                cb = it->second.on_stdout;
            }

            if (!pipe || !procHandle) return;

            if (!ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) || bytesRead == 0) {
                break;
            }

            buffer[bytesRead] = '\0';
            lineBuffer += buffer;

            size_t pos;
            while ((pos = lineBuffer.find('\n')) != std::string::npos) {
                std::string line = lineBuffer.substr(0, pos);
                lineBuffer.erase(0, pos + 1);

                if (cb) cb(line);
            }

            // проверка что процесс жив
            DWORD code = 0;
            if (!GetExitCodeProcess(procHandle, &code) || code != STILL_ACTIVE) {
                break;
            }
        }

        // остаток строки
        if (!lineBuffer.empty()) {
            std::function<void(const std::string&)> cb;

            {
                std::lock_guard lock(mutex_);
                auto it = processes_.find(pid);
                if (it != processes_.end())
                    cb = it->second.on_stdout;
            }

            if (cb) cb(lineBuffer);
        }
    });
}

// ─────────────────────────────
// exit thread
// ─────────────────────────────

void ProcessManager::start_exit_thread(Pid pid) {
    std::lock_guard lock(mutex_);

    auto it = processes_.find(pid);
    if (it == processes_.end()) return;

    it->second.exit_thread = std::thread([this, pid]() {
        HANDLE hProcess = nullptr;

        {
            std::lock_guard lock(mutex_);
            auto it = processes_.find(pid);
            if (it == processes_.end()) return;

            hProcess = it->second.pi.hProcess;
        }

        if (!hProcess) return;

        WaitForSingleObject(hProcess, INFINITE);

        std::function<void()> cb;

        {
            std::lock_guard lock(mutex_);
            auto it = processes_.find(pid);
            if (it == processes_.end()) return;

            cb = it->second.on_exit;
        }

        if (cb) cb();

        cleanup(pid);
    });
}

// ─────────────────────────────
// stdin
// ─────────────────────────────

void ProcessManager::write_stdin(Pid pid, const std::string& data) {
    std::lock_guard lock(mutex_);

    auto it = processes_.find(pid);
    if (it == processes_.end()) return;

    if (!it->second.stdin_write) return;

    DWORD written = 0;

    WriteFile(
        it->second.stdin_write,
        data.c_str(),
        static_cast<DWORD>(data.size()),
        &written,
        nullptr
    );
}

// ─────────────────────────────
// stop (force kill)
// ─────────────────────────────

void ProcessManager::stop(Pid pid) {
    HANDLE h = nullptr;

    {
        std::lock_guard lock(mutex_);
        auto it = processes_.find(pid);
        if (it == processes_.end()) return;

        h = it->second.pi.hProcess;
    }

    if (h) {
        TerminateProcess(h, 0);
    }
}

// ─────────────────────────────
// is_running
// ─────────────────────────────

bool ProcessManager::is_running(Pid pid) {
    std::lock_guard lock(mutex_);

    auto it = processes_.find(pid);
    if (it == processes_.end()) return false;

    DWORD code = 0;
    if (GetExitCodeProcess(it->second.pi.hProcess, &code)) {
        return code == STILL_ACTIVE;
    }

    return false;
}

// ─────────────────────────────
// cleanup
// ─────────────────────────────

void ProcessManager::cleanup(Pid pid) {
    Process proc;

    {
        std::lock_guard lock(mutex_);
        auto it = processes_.find(pid);
        if (it == processes_.end()) return;

        proc = std::move(it->second);
        processes_.erase(it);
    }

    // ⬇️ реально вне mutex

    if (proc.stdout_thread.joinable() &&
        proc.stdout_thread.get_id() != std::this_thread::get_id())
    {
        proc.stdout_thread.detach();
    }

    if (proc.exit_thread.joinable() &&
        proc.exit_thread.get_id() != std::this_thread::get_id())
    {
        proc.exit_thread.detach();
    }

    if (proc.pi.hProcess) CloseHandle(proc.pi.hProcess);
    if (proc.pi.hThread)  CloseHandle(proc.pi.hThread);

    if (proc.stdin_write) CloseHandle(proc.stdin_write);
    if (proc.stdout_read) CloseHandle(proc.stdout_read);

    if (proc.job) CloseHandle(proc.job);
}