#include "./includes/minecraftservermanager.h"
#include "./includes/logger.h"

#include <iostream>
#include <vector>
#include <numeric>

MinecraftServerManager::MinecraftServerManager(const json& config_data) {
    try {
        ZeroMemory(&procInfo_, sizeof(procInfo_));
        load_config(config_data);
    } catch (const std::exception& e) {
        LOG_CRITICAL(std::string("Ошибка инициализации: ") + e.what(), "MC_INIT");
        throw;
    }
}

MinecraftServerManager::~MinecraftServerManager() {
    stop();

    /*rcon_connecting_ = false;
    if (rcon_thread_.joinable()) {
        rcon_thread_.join();
    }
    rcon_.disconnect();*/

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

std::string quote(const std::string& str) {
    return "\"" + str + "\"";
}

void MinecraftServerManager::load_config(json data) {
    try {
        // Проверка обязательных полей
        if (!data.contains("java") || !data["java"].contains("path")) {
            throw std::runtime_error("config.json: Не указан путь к Java");
        }
        if (!data.contains("server") || !data["server"].contains("directory")) {
            throw std::runtime_error("config.json: Не указана директория сервера");
        }

        // Загрузка параметров с проверкой
        config_.java_path = data["java"]["path"].get<std::string>();
        config_.server_dir = data["server"]["directory"].get<std::string>();
        config_.forge_args = data["server"]["forge_args"].get<std::string>();

        // Обработка JVM аргументов
        if (data["java"].contains("jvm_args") && data["java"]["jvm_args"].is_array()) {
            for (const auto& arg : data["java"]["jvm_args"]) {
                config_.jvm_args_vec.push_back(arg.get<std::string>());
            }
        }

        // Валидация конфигурации
        if (!fs::exists(config_.java_path)) {
            throw std::runtime_error("config.json: Java не найдена по указанному пути");
        }
        if (!fs::exists(config_.server_dir)) {
            throw std::runtime_error("config.json: Директория сервера не существует");
        }

        /* Загрузка RCON конфигурации
        if (data["server"].contains("rcon")) {
            auto rcon_cfg = data["server"]["rcon"];
            config_.rcon.enabled = rcon_cfg.value("enabled", false);
            config_.rcon.host = rcon_cfg.value("host", "localhost");
            config_.rcon.port = rcon_cfg.value("port", 25575);
            config_.rcon.password = rcon_cfg.value("password", "");
            config_.rcon.retry_interval = rcon_cfg.value("retry_interval", 5000);
            config_.rcon.max_retries = rcon_cfg.value("max_retries", 12);
            
            if (config_.rcon.enabled) {
                LOG_INFO("RCON включен: " + config_.rcon.host + ":" + 
                        std::to_string(config_.rcon.port), "CONFIG");
            }
        }*/

        // Собираем команду запуска
        std::ostringstream oss;
        oss << quote(config_.java_path) << " ";

        for (const auto& arg : config_.jvm_args_vec)
            oss << arg << " ";

        oss << quote(config_.forge_args) << " nogui";

        config_.full_command = oss.str();

        LOG_INFO("Конфигурация успешно загружена: "+ config_.full_command, "CONFIG");
    } catch (const json::exception& e) {
        throw std::runtime_error("Ошибка JSON: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw std::runtime_error("Ошибка загрузки конфига: " + std::string(e.what()));
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

/*
void MinecraftServerManager::setup_rcon() {
    if (!config_.rcon.enabled || config_.rcon.password.empty()) {
        rcon_enabled_ = false;
        return;
    }
    
    rcon_enabled_ = true;
    rcon_connecting_ = true;
    
    // Запускаем поток для подключения RCON
    rcon_thread_ = std::thread(&MinecraftServerManager::rcon_connection_worker, this);
}

void MinecraftServerManager::rcon_connection_worker() {
    if (!rcon_enabled_) return;
    
    LOG_INFO("Запуск RCON подключения к " + config_.rcon.host + ":" + 
             std::to_string(config_.rcon.port), "RCON");
    
    // ВРЕМЕННАЯ ОТЛАДКА: выводим пароль (осторожно!)
    std::string masked_password = std::string(config_.rcon.password.size(), '*');
    LOG_DEBUG("Используемый пароль RCON: " + masked_password + " (длина: " + 
             std::to_string(config_.rcon.password.length()) + ")", "RCON");
    
    int retries = 0;
    while (rcon_connecting_ && retries < config_.rcon.max_retries) {
        if (rcon_.connect(config_.rcon.host, config_.rcon.port, config_.rcon.password)) {
            LOG_INFO("RCON успешно подключен", "RCON");
            check_server_ready_via_rcon();
            return;
        }
        
        retries++;
        LOG_WARNING("Попытка RCON подключения " + std::to_string(retries) + 
                   "/" + std::to_string(config_.rcon.max_retries) + " failed", "RCON");
        
        // Ждем перед следующей попыткой
        for (int i = 0; i < config_.rcon.retry_interval / 1000 && rcon_connecting_; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    if (rcon_connecting_) {
        LOG_ERR("Не удалось подключиться к RCON после всех попыток", "RCON");
        rcon_enabled_ = false;
    }
}

void MinecraftServerManager::check_server_ready_via_rcon() {
    if (!rcon_.is_connected()) return;
    
    try {
        auto response = rcon_.send_command("list");
        if (!response.empty()) {
            ready_ = true;
            status_ = ServerStatus::Running;
            LOG_INFO("Сервер полностью запущен (подтверждено через RCON)!", "MC");
            LOG_INFO("Ответ сервера: " + response, "MC");
        }
    } catch (const std::exception& e) {
        LOG_WARNING("Ошибка проверки готовности через RCON: " + std::string(e.what()), "MC");
    }
}
*/

/* ------------------------------------------------------------------ */
/*                               START                                */
/* ------------------------------------------------------------------ */
void MinecraftServerManager::start() {
    if (running_) {
        LOG_WARNING("Сервер уже запущен.", "MC");
        return;
    }

    // safety‑net, если кто‑то вызвал start() без stop()
    if (output_thread_.joinable())          output_thread_.join();
    if (process_monitor_thread_.joinable()) process_monitor_thread_.join();

    output_thread_          = std::thread();
    process_monitor_thread_ = std::thread();

    /* Сбрасываем procInfo_ на случай повторного запуска */
    ZeroMemory(&procInfo_, sizeof(procInfo_));

    status_ = ServerStatus::Starting;
    LOG_INFO("Запуск Minecraft‑сервера...", "MC");

    const std::string& cmd = config_.full_command;
    LOG_INFO("Конфиги запуска инициализированы!", "MC");

    /* ---------- Настройка пайпов ---------- */
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };

    HANDLE writePipeOut = nullptr;   // то, куда сервер пишет stdout
    HANDLE readPipeIn   = nullptr;   // то, откуда сервер читает stdin

    /* stdout → наш readPipe_ */
    if (!CreatePipe(&readPipe_, &writePipeOut, &sa, 0)) {
        LOG_ERR("Не удалось создать pipe stdout.", "MC_PIPE");
        status_ = ServerStatus::Stopped;
        return;
    }
    SetHandleInformation(readPipe_, HANDLE_FLAG_INHERIT, 0);

    /* stdin  ← наш stdinPipe_  */
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

    // Преобразуем рабочую директорию в C-строку
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

        LOG_CRITICAL("Прочитанный конфиг: " + config_.full_command ,"MC");

        status_ = ServerStatus::Stopped;
        running_ = false;
        ready_ = false;                   

        CloseHandle(readPipeIn);
        CloseHandle(stdinPipe_);
        CloseHandle(readPipe_);
        CloseHandle(writePipeOut);
        return;
    }

    /* Эти хендлы процессу больше не нужны */
    CloseHandle(writePipeOut);
    CloseHandle(readPipeIn);

    running_ = true;
    ready_   = false;

    LOG_INFO("Процесс сервера запущен успешно", "MC");

    //setup_rcon();

    /* ---------- Запускаем рабочие потоки ---------- */
    output_thread_          = std::thread(&MinecraftServerManager::read_output,          this);
    process_monitor_thread_ = std::thread(&MinecraftServerManager::monitor_process_exit, this);
}

/* ------------------------------------------------------------------
                                STOP                                */
                                
void MinecraftServerManager::stop() {
    if (!running_) return;

    status_ = ServerStatus::Stopping;
    LOG_INFO("Отправка 'stop' в stdin...", "MC");

    const std::string stopCmd = "stop\n";
    DWORD written;
    WriteFile(stdinPipe_, stopCmd.c_str(),
              static_cast<DWORD>(stopCmd.size()),
              &written, nullptr);

    /* Даём серверу шанс завершиться красиво */
    if (WaitForSingleObject(procInfo_.hProcess, 40'000) == WAIT_TIMEOUT) {
        LOG_WARNING("Сервер не вышел вовремя. Принудительное завершение...", "MC");
        TerminateProcess(procInfo_.hProcess, 0);
        WaitForSingleObject(procInfo_.hProcess, 5'000);
    }

    /* Stream‑потоки завершаются сами, но join обязателен */
    if (output_thread_.joinable())          output_thread_.join();
    if (process_monitor_thread_.joinable()) process_monitor_thread_.join();

    /* --- обнуляем --- */
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

    /*
    if (rcon_enabled_ && rcon_.is_connected()) {
        auto response = rcon_.send_command(cmd);
        if (!response.empty()) {
            LOG_DEBUG("Команда через RCON: " + cmd + " → " + response, "MC_RCON");
            return;
        } else {
            LOG_WARNING("RCON команда не получила ответа, пробуем через консоль", "MC_RCON");
        }
    }*/

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

                    // НЕ Дублируем в консоль, чтобы админ видел live‑лог.
                    //std::cout << "[MC] " << line << '\n';

                    // Пишем ПОЛНЫЙ вывод сервера в файл/консоль через Logger
                    LOG_INFO(line, "MC_OUT");

                    if (line.find("Dedicated server took") != std::string::npos && 
                        line.find("seconds to load") != std::string::npos) {
                        // Сервер запущен, но готовность подтвердим через RCON
                         status_ = ServerStatus::Running;
                         ready_ = true;
                        LOG_INFO("Сервер сообщил о готовности, проверяем через RCON...", "MC");
                    } else if (line.find("Stopping server") != std::string::npos) {
                        status_ = ServerStatus::Stopping;
                        LOG_INFO("Обнаружена остановка сервера...", "MC");
                    } else if (line.find("All dimensions are saved") != std::string::npos) {
                        running_ = false;
                        //rcon_connecting_ = false; // Останавливаем попытки RCON подключения
                        //rcon_.disconnect();
                    }
                }
            } else {
                break;  // ReadFile вернул 0 — пайп закрыт
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
        LOG_WARNING("Процесс сервера завершился извне.", "MC");

        /* Закрываем хендлы — только тут, чтобы не было дублей */
        if (procInfo_.hProcess) { CloseHandle(procInfo_.hProcess); procInfo_.hProcess = nullptr; }
        if (procInfo_.hThread)  { CloseHandle(procInfo_.hThread);  procInfo_.hThread  = nullptr; }
        if (stdinPipe_)         { CloseHandle(stdinPipe_);         stdinPipe_         = nullptr; }
        if (readPipe_)          { CloseHandle(readPipe_);          readPipe_          = nullptr; }

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
