#include "caddy_manager.h"
#include "../includes/logger.h"

#include <vector>

/* ------------------------------------------------------------------ */
/*                     CONSTRUCTOR / DESTRUCTOR                        */
/* ------------------------------------------------------------------ */
CaddyManager::CaddyManager(const json& config_data) {
    ZeroMemory(&procInfo_, sizeof(procInfo_));
    load_config(config_data);
}

CaddyManager::~CaddyManager() {
    stop();
    if (monitor_thread_.joinable()) monitor_thread_.join();

    std::lock_guard lg(handle_mutex_);
    if (procInfo_.hProcess) { CloseHandle(procInfo_.hProcess); procInfo_.hProcess = nullptr; }
    if (procInfo_.hThread)  { CloseHandle(procInfo_.hThread);  procInfo_.hThread  = nullptr; }
}

/* ------------------------------------------------------------------ */
/*                            CONFIG                                   */
/* ------------------------------------------------------------------ */
void CaddyManager::load_config(const json& data) {
    if (!data.contains("caddy")) {
        LOG_WARNING("config.json: секция 'caddy' не найдена, Caddy отключён", "CADDY");
        config_.enabled = false;
        return;
    }

    const auto& caddy = data["caddy"];
    config_.enabled     = caddy.value("enabled", false);
    config_.exe_path    = caddy.value("exe_path", "");
    config_.config_path = caddy.value("config_path", "");
    config_.working_dir = caddy.value("working_dir", "");

    if (config_.enabled && !config_.is_valid()) {
        LOG_ERR("Caddy конфигурация невалидна (проверьте пути в config.json)", "CADDY");
        config_.enabled = false;
        return;
    }

    if (config_.enabled) {
        LOG_INFO("Caddy конфигурация загружена: " + config_.exe_path, "CADDY");
    } else {
        LOG_INFO("Caddy отключён в конфиге", "CADDY");
    }
}

void CaddyManager::reload_config(const json& config_data) {
    try {
        load_config(config_data);
        LOG_INFO("Конфигурация Caddy перечитана", "CADDY");
    } catch (const std::exception& e) {
        LOG_ERR(std::string("Ошибка перечитывания конфига Caddy: ") + e.what(), "CADDY");
    }
}

/* ------------------------------------------------------------------ */
/*                          VALIDATE                                   */
/* ------------------------------------------------------------------ */
bool CaddyManager::validate_config() {
    std::string cmd = "\"" + config_.exe_path + "\" validate --config \"" + config_.config_path + "\"";
    LOG_INFO("Валидация Caddyfile...", "CADDY");

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    std::vector<char> dirBuf(config_.working_dir.begin(), config_.working_dir.end());
    dirBuf.push_back('\0');

    if (!CreateProcessA(nullptr, cmdBuf.data(),
                        nullptr, nullptr, FALSE, 0,
                        nullptr, dirBuf.data(),
                        &si, &pi))
    {
        DWORD err = GetLastError();
        LOG_ERR("Не удалось запустить caddy validate (код " + std::to_string(err) + "): "
                + get_last_error_message(err), "CADDY");
        return false;
    }

    WaitForSingleObject(pi.hProcess, 30000);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        LOG_ERR("Caddyfile валидация провалилась (exit code " + std::to_string(exitCode) + ")", "CADDY");
        return false;
    }

    LOG_INFO("Caddyfile валидация успешна", "CADDY");
    return true;
}

/* ------------------------------------------------------------------ */
/*                            START                                    */
/* ------------------------------------------------------------------ */
void CaddyManager::start() {
    if (running_) {
        LOG_WARNING("Caddy уже запущен", "CADDY");
        return;
    }

    if (!config_.enabled) {
        LOG_WARNING("Caddy отключён в конфиге, запуск отменён", "CADDY");
        return;
    }

    if (monitor_thread_.joinable()) monitor_thread_.join();
    monitor_thread_ = std::thread();

    ZeroMemory(&procInfo_, sizeof(procInfo_));
    status_ = CaddyStatus::Starting;

    if (!validate_config()) {
        LOG_ERR("Caddy не запущен из-за ошибок в Caddyfile", "CADDY");
        status_ = CaddyStatus::Stopped;
        return;
    }

    std::string cmd = "\"" + config_.exe_path + "\" run --config \"" + config_.config_path + "\"";
    LOG_INFO("Запуск Caddy: " + cmd, "CADDY");

    // Перенаправляем stdout/stderr Caddy в NUL, чтобы не спамил в консоль
    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE hNul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = INVALID_HANDLE_VALUE;
    si.hStdOutput = hNul;
    si.hStdError  = hNul;

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    std::vector<char> dirBuf(config_.working_dir.begin(), config_.working_dir.end());
    dirBuf.push_back('\0');

    if (!CreateProcessA(nullptr, cmdBuf.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_NEW_PROCESS_GROUP,
                        nullptr, dirBuf.data(),
                        &si, &procInfo_))
    {
        DWORD err = GetLastError();
        LOG_CRITICAL("Ошибка запуска Caddy (код " + std::to_string(err) + "): "
                     + get_last_error_message(err), "CADDY");
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        status_ = CaddyStatus::Stopped;
        return;
    }

    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    running_ = true;
    status_  = CaddyStatus::Running;

    LOG_INFO("Caddy запущен, PID: " + std::to_string(procInfo_.dwProcessId), "CADDY");

    monitor_thread_ = std::thread(&CaddyManager::monitor_process_exit, this);
}

/* ------------------------------------------------------------------ */
/*                             STOP                                    */
/* ------------------------------------------------------------------ */
void CaddyManager::stop() {
    if (!running_) return;

    status_ = CaddyStatus::Stopping;
    running_ = false;
    LOG_INFO("Остановка Caddy...", "CADDY");

    {
        std::lock_guard lg(handle_mutex_);
        if (procInfo_.hProcess) {
            TerminateProcess(procInfo_.hProcess, 0);
            WaitForSingleObject(procInfo_.hProcess, 5000);
        }
    }

    if (monitor_thread_.joinable()) monitor_thread_.join();
    monitor_thread_ = std::thread();

    LOG_INFO("Caddy остановлен", "CADDY");
}

/* ------------------------------------------------------------------ */
/*                           RESTART                                   */
/* ------------------------------------------------------------------ */
void CaddyManager::restart() {
    LOG_INFO("Перезапуск Caddy...", "CADDY");
    stop();
    start();
}

/* ------------------------------------------------------------------ */
/*                          STATUS / QUERIES                           */
/* ------------------------------------------------------------------ */
bool CaddyManager::is_running() const {
    return running_;
}

CaddyStatus CaddyManager::get_status() const {
    return status_;
}

bool CaddyManager::is_enabled() const {
    return config_.enabled;
}

/* ------------------------------------------------------------------ */
/*                      MONITOR PROCESS EXIT                           */
/* ------------------------------------------------------------------ */
void CaddyManager::monitor_process_exit() {
    try {
        if (!procInfo_.hProcess) return;

        WaitForSingleObject(procInfo_.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(procInfo_.hProcess, &exitCode);

        {
            std::lock_guard lg(handle_mutex_);
            if (procInfo_.hProcess) { CloseHandle(procInfo_.hProcess); procInfo_.hProcess = nullptr; }
            if (procInfo_.hThread)  { CloseHandle(procInfo_.hThread);  procInfo_.hThread  = nullptr; }
        }

        bool wasRunning = running_.exchange(false);
        status_ = CaddyStatus::Stopped;

        if (wasRunning) {
            LOG_WARNING("Caddy завершился неожиданно (exit code " + std::to_string(exitCode) + ")", "CADDY");
        } else {
            LOG_INFO("Caddy процесс завершён", "CADDY");
        }
    } catch (const std::exception& ex) {
        LOG_ERR(std::string("[caddy monitor] Exception: ") + ex.what(), "CADDY");
    } catch (...) {
        LOG_ERR("[caddy monitor] Unknown exception", "CADDY");
    }
}

/* ------------------------------------------------------------------ */
/*                        WIN32 ERROR HELPER                           */
/* ------------------------------------------------------------------ */
std::string CaddyManager::get_last_error_message(DWORD err) {
    LPSTR buf = nullptr;
    size_t sz = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);

    std::string msg(buf, sz);
    LocalFree(buf);

    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();

    return msg;
}
