#define _CRT_SECURE_NO_WARNINGS
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING

#include "./includes/httpServer.h"
#include <io.h>
#include <algorithm>
#include <fstream>
#include <windows.h>
#include <deque>
#include <codecvt>
#include <limits>
#include <ws2tcpip.h>

#include "./includes/logger.h"

using json = nlohmann::json;

std::string status_to_string(ServerStatus status) {
    switch (status) {
        case ServerStatus::Stopped: return "Остановлен";
        case ServerStatus::Starting: return "Запускаю...";
        case ServerStatus::Running: return "Запущен";
        case ServerStatus::Stopping: return "Останавливаю...";
        default: return "хз, ЫсчЭз. наелся и спит";
    }
}

HttpServer::HttpServer(MinecraftServerManager& manager, 
    int port, 
    std::atomic<bool>& running, 
    const std::string& tokens_file,
    const std::string& logs_path,
    const std::string& modpack_path,
    const std::string& web_root,
    int upload_limit)
    : manager_(manager), 
      port_(port), 
      running_(running), 
      tokens_file_(tokens_file),
      logs_path_(logs_path),
      modpack_path_(modpack_path),
      web_root_(web_root),
      upload_limit_(upload_limit * 1024 * 1024)
{
    load_tokens();
}

void HttpServer::load_tokens() {
    std::lock_guard lg(tokens_mx_);
    std::unordered_set<std::string> fresh;
    std::ifstream f(tokens_file_);
    if (!f) {
        LOG_ERR("Не смог открыть " + tokens_file_, "WEB");
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty() && line[0] != '#') fresh.insert(line);
    }
    tokens_.swap(fresh);
    LOG_INFO("Токены перечитаны, всего: " + std::to_string(tokens_.size()), "WEB");
}

bool HttpServer::check_token(const std::string& t) {
    std::lock_guard lg(tokens_mx_);
    return tokens_.count(t) > 0;
}

json HttpServer::get_status_json() {
    ServerStatus status = manager_.get_status();

    return json{
        {"status", status_to_string(status)},
        {"ip", "91.223.70.49"},
        {"port", 25566},
        {"version", "Forge 1.20.1"}
    };
}
// ────────────────────────────────────────────────────────────────
//  sanitize_utf8  —   пропускает только корректные UTF‑8 байты
// ────────────────────────────────────────────────────────────────
static std::string sanitize_utf8(const std::string& in)
{
    std::string out;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(in.data());
    size_t i = 0, n = in.size();

    while (i < n)
    {
        unsigned char c = s[i];

        // ASCII
        if (c < 0x80) { out.push_back(c); ++i; continue; }

        // 2‑байт U+0080..07FF
        if ((c >> 5) == 0x6 && i+1 < n && (s[i+1] & 0xC0) == 0x80)
        { out.append(reinterpret_cast<const char*>(s+i), 2); i += 2; continue; }

        // 3‑байт U+0800..FFFF
        if ((c >> 4) == 0xE && i+2 < n &&
            (s[i+1] & 0xC0) == 0x80 && (s[i+2] & 0xC0) == 0x80)
        { out.append(reinterpret_cast<const char*>(s+i), 3); i += 3; continue; }

        // 4‑байт U+10000..10FFFF
        if ((c >> 3) == 0x1E && i+3 < n &&
            (s[i+1] & 0xC0) == 0x80 && (s[i+2] & 0xC0) == 0x80 &&
            (s[i+3] & 0xC0) == 0x80)
        { out.append(reinterpret_cast<const char*>(s+i), 4); i += 4; continue; }

        // иначе — битый байт, пропускаем
        ++i;
    }
    return out;
}


void HttpServer::run() {
    LOG_INFO("Инициализация маршрутов...", "WEB");

    if (!std::ifstream(logs_path_)) {
        LOG_ERR("Лог-файл не найден: " + logs_path_, "WEB");
    }
    if (!std::ifstream(modpack_path_)) {
        LOG_ERR("Файл сборки модов не найден: " + modpack_path_, "WEB");
    }
    if (!fs::exists(web_root_)) {
        LOG_ERR("Директория с сайтом не найдена: " + web_root_, "WEB");
    }

    LOG_INFO("Адрес памяти web-сервера: " + std::to_string(reinterpret_cast<uintptr_t>(&svr)), "WEB");

    // Middleware токена
    svr.set_pre_routing_handler([&](const auto& req, auto& res) {
        std::string client_ip = req.get_header_value("X-Real-IP");
        if (client_ip.empty()) client_ip = req.get_header_value("X-Forwarded-For");
        if (client_ip.empty()) client_ip = req.remote_addr;

        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_req;
        auto ip = client_ip;
        
        if (last_req.count(ip) && 
            std::chrono::steady_clock::now() - last_req[ip] < std::chrono::milliseconds(1000)) {
            res.status = 429;
            return httplib::Server::HandlerResponse::Handled;
        }

        static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;

        std::wstring wmethod = conv.from_bytes(req.method);
        std::wstring wpath   = conv.from_bytes(req.path);

        LOG_INFO("[" + client_ip + "] " + req.method + " " + req.path, "WEB");

        if (req.path.rfind("/api/", 0) == 0) {
            auto token = req.get_header_value("X-API-Token");

            if (token.empty()) {
                auto it = req.params.find("token");
                if (it != req.params.end()) {
                    token = it->second;
                }
            }
            if (!check_token(token)) {
                res.status = 401;
                res.set_content("Unauthorized", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
        }

        std::string proto = req.get_header_value("X-Forwarded-Proto");
        if (proto.empty()) proto = req.get_header_value("X-Forwarded-Scheme");

        // Редирект HTTPS
        if (proto != "https" && !proto.empty()) {
            res.status = 403;
            res.set_content("HTTPS required", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Эндпоинты API
    svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        json response = get_status_json();
        res.set_content(response.dump(), "application/json");
    });

    svr.Get("/api/logs", [this](const httplib::Request& req, httplib::Response& res) {
        std::ifstream log;

        int attempts = 3;
        while (attempts-- > 0) {
            log.open(logs_path_);
            if (log.is_open()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!log.is_open()) {
            LOG_ERR("Не удалось открыть logPath: " + logs_path_ + ", errno=" + std::to_string(errno), "WEB");
            res.status = 404;
            res.set_content(R"({"error": "Лог-файл не найден или недоступен"})", "application/json");
            return;
        }

        try {
            // Оптимизированное чтение последних строк
            const size_t maxLines = 500;
            std::deque<std::string> lastLines;
            
            // Простой способ для небольших файлов
            std::string line;
            while (std::getline(log, line)) {
                line = sanitize_utf8(line); 
                if (lastLines.size() >= maxLines) {
                    lastLines.pop_front();
                }
                lastLines.push_back(line);
            }

            // Формируем ответ
            std::string logs;
            for (const auto& l : lastLines) {
                logs += l + "\n";
            }

            json response = { {"logs", sanitize_utf8(logs)} };   // ← ещё раз на всяк случай
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG_ERR("Ошибка при чтении логов: " + std::string(e.what()), "WEB");
            res.status = 500;
            res.set_content(R"({"error": "Ошибка при чтении лог-файла"})", "application/json");
        }
    });

    svr.Get("/api/download-modpack", [this](const httplib::Request& req, httplib::Response& res) {
        // 1. Проверяем существование файла
        if (!std::filesystem::exists(modpack_path_)) {
            LOG_ERR("Файл не существует: " + modpack_path_, "WEB");
            res.status = 404;
            res.set_content("Файл не найден", "text/plain");
            return;
        }

        // 2. Открываем файл
        auto file_stream = std::make_shared<std::ifstream>(
            modpack_path_, 
            std::ios::binary | std::ios::ate
        );
        
        if (!*file_stream) {
            LOG_ERR("Не удалось открыть файл сборки: " + modpack_path_, "WEB");
            res.status = 500;
            res.set_content("Ошибка открытия файла", "text/plain");
            return;
        }

        // 3. Получаем размер файла
        auto file_size = file_stream->tellg();
        file_stream->seekg(0);

        // 4. Устанавливаем заголовки
        res.set_header("Content-Type", "application/zip");
        res.set_header("Content-Disposition", 
            "attachment; filename=" + std::filesystem::path(modpack_path_).filename().string());
        res.set_header("Content-Length", std::to_string(file_size));
        res.set_header("Cache-Control", "no-cache");

        // 5. Создаем буфер (размер можно подобрать экспериментально)
        constexpr size_t buffer_size = 64 * 1024;  // 64 KB
        auto buffer = std::make_shared<std::vector<char>>(buffer_size);

        // 6. Функция для потоковой передачи с ограничением скорости
        auto provider = [
            file_stream,
            buffer,
            upload_limit = this->upload_limit_,
            start_time = std::chrono::steady_clock::now(),
            bytes_sent = size_t(0)
        ](size_t offset, size_t length, httplib::DataSink& sink) mutable -> bool {
            try {
                file_stream->seekg(static_cast<std::streamoff>(offset));
                
                while (length > 0 && !file_stream->eof()) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - start_time).count();
                    
                    // Рассчитываем максимальное количество байт, которое можно отправить
                    size_t max_allowed = upload_limit > 0 
                        ? (upload_limit * (elapsed + 1) / 1000) - bytes_sent
                        : (std::numeric_limits<size_t>::max)();
                    
                    if (max_allowed <= 0) {
                        // Превышен лимит скорости, ждём
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    
                    // Читаем не больше доступной квоты и размера буфера
                    auto read_size = static_cast<std::streamsize>(
                        std::min<size_t>({length, buffer->size(), max_allowed})
                    );
                    
                    file_stream->read(buffer->data(), read_size);
                    auto count = file_stream->gcount();
                    
                    if (count <= 0) break;
                    
                    if (!sink.write(buffer->data(), static_cast<size_t>(count))) {
                        return false; // Клиент отключился
                    }
                    
                    bytes_sent += static_cast<size_t>(count);
                    length -= static_cast<size_t>(count);
                }

                if (length == 0 || file_stream->eof()) {
                    sink.done();
                    return true;
                }
            } catch (const std::exception& e) {
                return false;
            }
            
            return false;
        };

        // 7. Устанавливаем провайдер контента
        res.set_content_provider(
            static_cast<size_t>(file_size),
            "application/zip",
            provider
        );
    });

    svr.Post("/api/start", [this](const httplib::Request&, httplib::Response& res) {
        std::wcout << L"Запрошен запуск" << std::endl;
        manager_.start();
        res.set_content(get_status_json().dump(), "application/json");
    });

    svr.Post("/api/stop", [this](const httplib::Request&, httplib::Response& res) {
        manager_.stop();
        res.set_content(get_status_json().dump(), "application/json");
    });

    svr.Post("/api/exit", [this](const httplib::Request&, httplib::Response& res) {
        std::wcout << L"Получен запрос на завершение работы через API" << std::endl;
        /*manager_.stop();
        this->stop();
        running_ = false;
        #ifdef _WIN32
            // 1) берём дескриптор консольного stdin
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            // 2) отменяем все операции чтения (ReadConsole) в другом потоке
            CancelIoEx(hIn, nullptr);
            // 3) закрываем, чтобы дальнейшие чтения сразу давали EOF
            CloseHandle(hIn);
        #endif*/
        res.set_content(R"({
        "status": "shutdown",
        "message": "Сервер выключается. До скорой встречи!(нет) "
        })", "application/json");

    });

    svr.Post("/api/command", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            manager_.send_command(body["command"].get<std::string>());
            res.set_content(get_status_json().dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json{{"error", "invalid request"}}.dump(), "application/json");
        }
    });

    // Фронтенд
    svr.set_mount_point("/", web_root_);
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/index.html");
    });

    LOG_INFO("HTTP сервер запущен на порту: " + std::to_string(port_), "WEB");
    try {
        svr.new_task_queue = [] { return new httplib::ThreadPool(4); };
        if (!svr.listen("0.0.0.0", port_)) {
            LOG_ERR("Не удалось запустить сервер!", "WEB");
        }
    } catch (const std::exception& ex) {
        LOG_ERR(std::string("Ошибка: ") + ex.what(), "WEB");
        //std::wcerr << L"[WEB] Ошибка: " << ex.what() << std::endl;
    }

}

void HttpServer::stop() {
    LOG_WARNING("Остановка WEB сервера...", "WEB");
    svr.stop();

#ifdef _WIN32
    // Форсируем разрыв select() через самоподключение
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock != INVALID_SOCKET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = htons(port_);
        connect(sock, (sockaddr*)&addr, sizeof(addr));
        closesocket(sock);
    }
#endif
    LOG_INFO("Сервер остановлен", "WEB");
}