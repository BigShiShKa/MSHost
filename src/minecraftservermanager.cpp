#include "./includes/minecraftservermanager.h"
#include "./includes/logger.h"

#include <iostream>
#include <vector>
#include <numeric>

MinecraftServerManager::MinecraftServerManager(const json& config_data) {
    try {
        ZeroMemory(&procInfo_, sizeof(procInfo_));
        load_config(config_data);

        if (!config_.is_valid()) {
            throw std::runtime_error("Конфигурация невалидна после загрузки");
        }
    } catch (const std::exception& e) {
        LOG_CRITICAL(std::string("Ошибка инициализации: ") + e.what(), "MC_INIT");
        throw;
    }
}

MinecraftServerManager::~MinecraftServerManager() {
    stop();

    // Ждём завершения потоков перед закрытием хендлов
    if (output_thread_.joinable())          output_thread_.join();
    if (process_monitor_thread_.joinable()) process_monitor_thread_.join();

    // Единственное место закрытия хендлов (в деструкторе, после join)
    std::lock_guard lg(handle_mutex_);
    if (procInfo_.hProcess) {
        CloseHandle(procInfo_.hProcess);
        procInfo_.hProcess = nullptr;
    }
    if (procInfo_.hThread) {
        CloseHandle(procInfo_.hThread);
        procInfo_.hThread = nullptr;
    }
    if (stdinPipe_) {
        CloseHandle(stdinPipe_);
        stdinPipe_ = nullptr;
    }
    if (readPipe_) {
        CloseHandle(readPipe_);
        readPipe_ = nullptr;
    }
}

static std::string quote(const std::string& str) {
    return "\"" + str + "\"";
}

void MinecraftServerManager::load_config(json data) {
    try {
        if (!data.contains("java") || !data["java"].contains("path")) {
            throw std::runtime_error("config.json: Не указан путь к Java");
        }
        if (!data.contains("server") || !data["server"].contains("directory")) {
            throw std::runtime_error("config.json: Не указана директория сервера");
        }

        config_.java_path  = data["java"]["path"].get<std::string>();
        config_.server_dir = data["server"]["directory"].get<std::string>();
        config_.forge_args = data["server"]["forge_args"].get<std::string>();

        // JVM аргументы
        if (data["java"].contains("jvm_args") && data["java"]["jvm_args"].is_array()) {
            for (const auto& arg : data["java"]["jvm_args"]) {
                config_.jvm_args_vec.push_back(arg.get<std::string>());
            }
        }

        // user_jvm_args
        if (data["server"].contains("user_jvm_args")) {
            config_.user_jvm_args = data["server"]["user_jvm_args"].get<std::string>();
        }

        // Таймауты из конфига
        if (data["server"].contains("stop_timeout_ms")) {
            config_.stop_timeout_ms = data["server"]["stop_timeout_ms"].get<DWORD>();
        }
        if (data["server"].contains("force_kill_timeout_ms")) {
            config_.force_kill_timeout_ms = data["server"]["force_kill_timeout_ms"].get<DWORD>();
        }

        // Валидация путей
        if (!fs::exists(config_.java_path)) {
            throw std::runtime_error("config.json: Java не найдена по указанному пути");
        }
        if (!fs::exists(config_.server_dir)) {
            throw std::runtime_error("config.json: Директория сервера не существует");
        }

        // Собираем команду запуска
        std::ostringstream oss;
        oss << quote(config_.java_path) << " ";
        for (const auto& arg : config_.jvm_args_vec)
            oss << arg << " ";
        if (!config_.user_jvm_args.empty())
            oss << quote(config_.user_jvm_args) << " ";
        oss << quote(config_.forge_args) << " nogui";
        config_.full_command = oss.str();

        // RCON конфигурация
        if (data["server"].contains("rcon")) {
            const auto& rcon = data["server"]["rcon"];
            rcon_config_.enabled          = rcon.value("enabled", false);
            rcon_config_.host             = rcon.value("host", "127.0.0.1");
            rcon_config_.port             = rcon.value("port", 25575);
            rcon_config_.password         = rcon.value("password", "");
            rcon_config_.retry_interval_ms = rcon.value("retry_interval", 3000);
            rcon_config_.max_retries      = rcon.value("max_retries", 12);
            if (rcon_config_.enabled) {
                LOG_INFO("RCON включён: " + rcon_config_.host + ":" + std::to_string(rcon_config_.port), "CONFIG");
            }
        }

        LOG_INFO("Конфигурация успешно загружена: " + config_.full_command, "CONFIG");
        LOG_INFO("Таймаут остановки: " + std::to_string(config_.stop_timeout_ms) +
                 " мс, принудительное завершение: " + std::to_string(config_.force_kill_timeout_ms) + " мс", "CONFIG");
    } catch (const json::exception& e) {
        throw std::runtime_error("Ошибка JSON: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error("Ошибка загрузки конфига: " + std::string(e.what()));
    }
}

void MinecraftServerManager::reload_config(const json& config_data) {
    try {
        config_.jvm_args_vec.clear();
        load_config(config_data);
        LOG_INFO("Конфигурация MC-сервера перечитана", "MC_INIT");
    } catch (const std::exception& e) {
        LOG_ERR(std::string("Ошибка перечитывания конфига MC: ") + e.what(), "MC_INIT");
    }
}

/* ---------- Получение текстовой расшифровки Win32-ошибки ---------- */
std::string MinecraftServerManager::get_last_error_message(DWORD err) {
    LPSTR buf = nullptr;
    const size_t sz = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                     FORMAT_MESSAGE_FROM_SYSTEM     |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, err,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPSTR>(&buf), 0, nullptr);

    std::string msg(buf, sz);
    LocalFree(buf);

    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();

    return msg;
}

/* ------------------------------------------------------------------ */
/*                               START                                */
/* ------------------------------------------------------------------ */
void MinecraftServerManager::start() {
    if (running_) {
        LOG_WARNING("Сервер уже запущен.", "MC");
        return;
    }

    // Ждём завершения потоков от предыдущей сессии
    if (output_thread_.joinable())          output_thread_.join();
    if (process_monitor_thread_.joinable()) process_monitor_thread_.join();

    output_thread_          = std::thread();
    process_monitor_thread_ = std::thread();

    ZeroMemory(&procInfo_, sizeof(procInfo_));

    status_ = ServerStatus::Starting;
    LOG_INFO("Запуск Minecraft-сервера...", "MC");

    const std::string& cmd = config_.full_command;
    LOG_INFO("Конфиги запуска инициализированы!", "MC");

    /* ---------- Настройка пайпов ---------- */
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };

    HANDLE writePipeOut = nullptr;
    HANDLE readPipeIn   = nullptr;

    if (!CreatePipe(&readPipe_, &writePipeOut, &sa, 0)) {
        LOG_ERR("Не удалось создать pipe stdout.", "MC_PIPE");
        status_ = ServerStatus::Stopped;
        return;
    }
    SetHandleInformation(readPipe_, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&readPipeIn, &stdinPipe_, &sa, 0)) {
        LOG_ERR("Не удалось создать pipe stdin.", "MC_PIPE");
        status_ = ServerStatus::Stopped;
        CloseHandle(readPipe_);
        CloseHandle(writePipeOut);
        return;
    }
    SetHandleInformation(stdinPipe_, HANDLE_FLAG_INHERIT, 0);

    /* ---------- Запуск процесса ---------- */
    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.hStdInput  = readPipeIn;
    si.hStdOutput = writePipeOut;
    si.hStdError  = writePipeOut;
    si.dwFlags    = STARTF_USESTDHANDLES;

    std::vector<char> dirBuf(config_.server_dir.begin(), config_.server_dir.end());
    dirBuf.push_back('\0');

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(nullptr, cmdBuf.data(),
                        nullptr, nullptr, TRUE, 0,
                        nullptr, dirBuf.data(),
                        &si, &procInfo_))
    {
        DWORD err = GetLastError();
        std::string errMsg = get_last_error_message(err);
        LOG_CRITICAL("Ошибка запуска (код " + std::to_string(err) + "): " + errMsg, "MC");
        LOG_CRITICAL("Прочитанный конфиг: " + config_.full_command, "MC");

        status_  = ServerStatus::Stopped;
        running_ = false;
        ready_   = false;

        CloseHandle(readPipeIn);
        CloseHandle(stdinPipe_);  stdinPipe_ = nullptr;
        CloseHandle(readPipe_);   readPipe_  = nullptr;
        CloseHandle(writePipeOut);
        return;
    }

    CloseHandle(writePipeOut);
    CloseHandle(readPipeIn);

    running_ = true;
    ready_   = false;

    LOG_INFO("Процесс сервера запущен успешно", "MC");

    output_thread_          = std::thread(&MinecraftServerManager::read_output,          this);
    process_monitor_thread_ = std::thread(&MinecraftServerManager::monitor_process_exit, this);
}

/* ------------------------------------------------------------------ */
/*                                STOP                                */
/* ------------------------------------------------------------------ */
void MinecraftServerManager::stop() {
    if (!running_) return;

    disconnect_rcon();

    status_ = ServerStatus::Stopping;
    LOG_INFO("Отправка 'stop' в stdin...", "MC");

    // Безопасная запись в пайп
    {
        std::lock_guard lg(pipe_mutex_);
        std::lock_guard hg(handle_mutex_);
        if (stdinPipe_) {
            const std::string stopCmd = "stop\n";
            DWORD written;
            WriteFile(stdinPipe_, stopCmd.c_str(),
                      static_cast<DWORD>(stopCmd.size()),
                      &written, nullptr);
        }
    }

    /* Даём серверу шанс завершиться красиво */
    if (procInfo_.hProcess) {
        if (WaitForSingleObject(procInfo_.hProcess, config_.stop_timeout_ms) == WAIT_TIMEOUT) {
            LOG_WARNING("Сервер не вышел вовремя. Принудительное завершение...", "MC");
            TerminateProcess(procInfo_.hProcess, 0);
            WaitForSingleObject(procInfo_.hProcess, config_.force_kill_timeout_ms);
        }
    }

    /* Ждём завершения потоков */
    if (output_thread_.joinable())          output_thread_.join();
    if (process_monitor_thread_.joinable()) process_monitor_thread_.join();

    output_thread_          = std::thread();
    process_monitor_thread_ = std::thread();

    LOG_INFO("Сервер остановлен.", "MC");
}

/* ------------------------------------------------------------------ */
/*                            ВСПОМОГАТЕЛЬНОЕ                         */
/* ------------------------------------------------------------------ */
bool MinecraftServerManager::is_running() const {
    return running_ && ready_;
}

ServerStatus MinecraftServerManager::get_status() const {
    return status_;
}

void MinecraftServerManager::send_command(const std::string& cmd) {
    if (!running_) {
        LOG_WARNING("Сервер не запущен — некуда слать команды.", "MC");
        return;
    }

    std::lock_guard lg(pipe_mutex_);
    std::lock_guard hg(handle_mutex_);

    if (!stdinPipe_) {
        LOG_ERR("Пайп stdin уже закрыт.", "MC_IO");
        return;
    }

    const std::string cmdNL = cmd + '\n';
    DWORD written;
    if (!WriteFile(stdinPipe_, cmdNL.c_str(),
                   static_cast<DWORD>(cmdNL.size()),
                   &written, nullptr))
    {
        LOG_ERR("Ошибка записи в stdin сервера.", "MC_IO");
    }
    else {
        LOG_DEBUG("Команда отправлена: " + cmd, "MC_IO");
    }
}

/* ---------- Чтение stdout сервера ---------- */
void MinecraftServerManager::read_output() {
    try {
        char buffer[1025];
        DWORD bytesRead;
        std::string lineBuf;

        while (running_) {
            DWORD avail = 0;
            if (!PeekNamedPipe(readPipe_, nullptr, 0, nullptr, &avail, nullptr)) {
                LOG_ERR("[read_output] Ошибка PeekNamedPipe.", "MC_IO");
                break;
            }
            if (avail == 0) {
                Sleep(10);
                continue;
            }

            if (ReadFile(readPipe_, buffer, 1024, &bytesRead, nullptr) && bytesRead) {
                buffer[bytesRead] = '\0';
                lineBuf += buffer;

                size_t pos;
                while ((pos = lineBuf.find('\n')) != std::string::npos) {
                    std::string line = lineBuf.substr(0, pos);
                    lineBuf.erase(0, pos + 1);

                    LOG_INFO(line, "MC_OUT");

                    if (line.find("Dedicated server took") != std::string::npos &&
                        line.find("seconds to load") != std::string::npos) {
                        status_ = ServerStatus::Running;
                        ready_ = true;
                        LOG_INFO("Сервер готов к работе!", "MC");

                        // Подключаем RCON в фоне
                        if (rcon_config_.enabled) {
                            std::thread([this]() { connect_rcon(); }).detach();
                        }
                    } else if (line.find("Stopping server") != std::string::npos) {
                        status_ = ServerStatus::Stopping;
                        LOG_INFO("Обнаружена остановка сервера...", "MC");
                    } else if (line.find("All dimensions are saved") != std::string::npos) {
                        running_ = false;
                    }
                }
            } else {
                break;
            }
        }
    } catch (const std::exception& ex) {
        LOG_CRITICAL(std::string("[read_output] Exception: ") + ex.what(), "MC_IO");
    } catch (...) {
        LOG_ERR("[read_output] Unknown exception.", "MC_IO");
    }
}

/* ---------- Мониторинг завершения процесса ---------- */
void MinecraftServerManager::monitor_process_exit() {
    try {
        if (!procInfo_.hProcess) return;

        WaitForSingleObject(procInfo_.hProcess, INFINITE);
        LOG_WARNING("Процесс сервера завершился.", "MC");

        disconnect_rcon();

        // Закрываем хендлы под мьютексом
        {
            std::lock_guard lg(handle_mutex_);
            if (procInfo_.hProcess) { CloseHandle(procInfo_.hProcess); procInfo_.hProcess = nullptr; }
            if (procInfo_.hThread)  { CloseHandle(procInfo_.hThread);  procInfo_.hThread  = nullptr; }
            if (stdinPipe_)         { CloseHandle(stdinPipe_);         stdinPipe_         = nullptr; }
            if (readPipe_)          { CloseHandle(readPipe_);          readPipe_          = nullptr; }
        }

        running_ = false;
        ready_   = false;
        status_  = ServerStatus::Stopped;

        LOG_INFO("Статус сервера: Stopped", "MC");
    } catch (const std::exception& ex) {
        LOG_ERR(std::string("[monitor] Exception: ") + ex.what(), "MC_IO");
    } catch (...) {
        LOG_ERR("[monitor] Unknown exception.", "MC_IO");
    }
}

/* ------------------------------------------------------------------ */
/*                               RCON                                  */
/* ------------------------------------------------------------------ */
void MinecraftServerManager::connect_rcon() {
    if (!rcon_config_.enabled) return;

    std::lock_guard lg(rcon_mutex_);

    for (int attempt = 0; attempt < rcon_config_.max_retries; ++attempt) {
        if (!running_) return;  // сервер уже выключается

        if (rcon_.connect(rcon_config_.host, rcon_config_.port, rcon_config_.password)) {
            rcon_connected_ = true;
            LOG_INFO("RCON подключение установлено", "RCON");
            return;
        }
        LOG_WARNING("RCON попытка " + std::to_string(attempt + 1) + "/" +
                    std::to_string(rcon_config_.max_retries) + " не удалась", "RCON");
        std::this_thread::sleep_for(std::chrono::milliseconds(rcon_config_.retry_interval_ms));
    }
    LOG_ERR("Не удалось подключиться к RCON после " +
            std::to_string(rcon_config_.max_retries) + " попыток", "RCON");
}

void MinecraftServerManager::disconnect_rcon() {
    std::lock_guard lg(rcon_mutex_);
    if (rcon_connected_) {
        rcon_.disconnect();
        rcon_connected_ = false;
        LOG_INFO("RCON отключен", "RCON");
    }
}

MinecraftServerManager::PlayerList MinecraftServerManager::get_players() {
    PlayerList result;
    if (!rcon_connected_ || !running_) return result;

    std::lock_guard lg(rcon_mutex_);
    std::string response = rcon_.send_command("list");

    if (response.empty()) return result;

    // Формат: "There are X of a max of Y players online: p1, p2, p3"
    auto are_pos = response.find("There are ");
    if (are_pos == std::string::npos) return result;

    auto num_start = are_pos + 10;
    auto of_pos = response.find(" of a max of ", num_start);
    if (of_pos == std::string::npos) return result;

    try {
        result.online = std::stoi(response.substr(num_start, of_pos - num_start));
    } catch (...) { return result; }

    auto max_start = of_pos + 13;
    auto players_pos = response.find(" players online", max_start);
    if (players_pos == std::string::npos) return result;

    try {
        result.max = std::stoi(response.substr(max_start, players_pos - max_start));
    } catch (...) { return result; }

    // Парсим имена после ": "
    auto colon_pos = response.find(": ", players_pos);
    if (colon_pos != std::string::npos) {
        std::string names_str = response.substr(colon_pos + 2);
        std::istringstream ss(names_str);
        std::string name;
        while (std::getline(ss, name, ',')) {
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if (!name.empty()) {
                result.names.push_back(name);
            }
        }
    }

    return result;
}

std::string MinecraftServerManager::rcon_command(const std::string& cmd) {
    if (!rcon_connected_ || !running_) return "";

    std::lock_guard lg(rcon_mutex_);
    return rcon_.send_command(cmd);
}
