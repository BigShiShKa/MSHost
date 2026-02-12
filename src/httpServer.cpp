#define _CRT_SECURE_NO_WARNINGS

#include "./includes/httpServer.h"
#include <algorithm>
#include <fstream>
#include <deque>
#include <limits>
#include <windows.h>
#include <ws2tcpip.h>

#include "./includes/logger.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// Роль текущего запроса (thread-local, httplib использует thread pool)
static thread_local std::string tl_current_role;

// ────────────────────────────────────────────────────────────────
// Статусы: локализованные строки + machine-readable коды
// ────────────────────────────────────────────────────────────────
static std::string status_to_string(ServerStatus status) {
    switch (status) {
        case ServerStatus::Stopped:  return "Остановлен";
        case ServerStatus::Starting: return "Запускаю...";
        case ServerStatus::Running:  return "Запущен";
        case ServerStatus::Stopping: return "Останавливаю...";
        default:                     return "Неизвестно";
    }
}

static std::string status_to_code(ServerStatus status) {
    switch (status) {
        case ServerStatus::Stopped:  return "stopped";
        case ServerStatus::Starting: return "starting";
        case ServerStatus::Running:  return "running";
        case ServerStatus::Stopping: return "stopping";
        default:                     return "unknown";
    }
}

// ────────────────────────────────────────────────────────────────
// sanitize_utf8 — пропускает только корректные UTF-8 байты
// ────────────────────────────────────────────────────────────────
static std::string sanitize_utf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    size_t i = 0, n = in.size();

    while (i < n) {
        unsigned char c = s[i];

        if (c < 0x80) { out.push_back(c); ++i; continue; }

        if ((c >> 5) == 0x6 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80)
        { out.append(reinterpret_cast<const char*>(s + i), 2); i += 2; continue; }

        if ((c >> 4) == 0xE && i + 2 < n &&
            (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80)
        { out.append(reinterpret_cast<const char*>(s + i), 3); i += 3; continue; }

        if ((c >> 3) == 0x1E && i + 3 < n &&
            (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 &&
            (s[i + 3] & 0xC0) == 0x80)
        { out.append(reinterpret_cast<const char*>(s + i), 4); i += 4; continue; }

        ++i; // битый байт — пропускаем
    }
    return out;
}

// ────────────────────────────────────────────────────────────────
// Конструктор
// ────────────────────────────────────────────────────────────────
HttpServer::HttpServer(MinecraftServerManager& manager,
                       std::atomic<bool>& running,
                       const WebConfig& config)
    : running_(running),
      manager_(manager),
      config_(config)
{
    load_tokens();
    setup_routes();
}

void HttpServer::load_tokens() {
    std::lock_guard lg(tokens_mx_);
    std::unordered_map<std::string, std::string> fresh;
    std::ifstream f(config_.tokens_file);
    if (!f) {
        LOG_ERR("Не смог открыть " + config_.tokens_file, "WEB");
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty() || line[0] == '#') continue;

        auto colon = line.rfind(':');
        if (colon != std::string::npos) {
            std::string token = line.substr(0, colon);
            std::string role  = line.substr(colon + 1);
            if (role != "admin" && role != "user") role = "admin";
            fresh[token] = role;
        } else {
            fresh[line] = "admin";  // обратная совместимость
        }
    }
    tokens_.swap(fresh);
    LOG_INFO("Токены перечитаны, всего: " + std::to_string(tokens_.size()), "WEB");
}

std::string HttpServer::check_token(const std::string& t) {
    std::lock_guard lg(tokens_mx_);
    auto it = tokens_.find(t);
    if (it != tokens_.end()) return it->second;
    return "";
}

json HttpServer::get_status_json() {
    ServerStatus status = manager_.get_status();
    return json{
        {"status",      status_to_string(status)},
        {"status_code", status_to_code(status)},
        {"ip",          config_.server_ip},
        {"port",        config_.server_port},
        {"version",     config_.server_version}
    };
}

void HttpServer::cleanup_rate_map() {
    auto now = std::chrono::steady_clock::now();
    auto threshold = std::chrono::milliseconds(config_.rate_limit_ms * 2);
    for (auto it = rate_map_.begin(); it != rate_map_.end(); ) {
        if (now - it->second > threshold) {
            it = rate_map_.erase(it);
        } else {
            ++it;
        }
    }
}

// ────────────────────────────────────────────────────────────────
// Регистрация маршрутов (отделена от run)
// ────────────────────────────────────────────────────────────────
void HttpServer::setup_routes() {
    LOG_INFO("Инициализация маршрутов...", "WEB");

    // Middleware: rate limit + аутентификация + HTTPS проверка
    svr.set_pre_routing_handler([this](const auto& req, auto& res) {
        std::string client_ip = req.get_header_value("X-Real-IP");
        if (client_ip.empty()) client_ip = req.get_header_value("X-Forwarded-For");
        if (client_ip.empty()) client_ip = req.remote_addr;

        // Rate limiting (потокобезопасный)
        {
            std::lock_guard lg(rate_mx_);

            auto now = std::chrono::steady_clock::now();
            auto it = rate_map_.find(client_ip);
            if (it != rate_map_.end() &&
                now - it->second < std::chrono::milliseconds(config_.rate_limit_ms)) {
                res.status = 429;
                return httplib::Server::HandlerResponse::Handled;
            }
            rate_map_[client_ip] = now;

            // Периодическая очистка (каждые 100 записей)
            if (rate_map_.size() > 100) {
                cleanup_rate_map();
            }
        }

        LOG_INFO("[" + client_ip + "] " + req.method + " " + req.path, "WEB");

        // Аутентификация API-эндпоинтов
        if (req.path.rfind("/api/", 0) == 0) {
            auto token = req.get_header_value("X-API-Token");
            if (token.empty()) {
                auto it = req.params.find("token");
                if (it != req.params.end()) {
                    token = it->second;
                }
            }
            std::string role = check_token(token);
            if (role.empty()) {
                res.status = 401;
                res.set_content("Unauthorized", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
            tl_current_role = role;
        }

        // HTTPS проверка
        std::string proto = req.get_header_value("X-Forwarded-Proto");
        if (proto.empty()) proto = req.get_header_value("X-Forwarded-Scheme");
        if (proto != "https" && !proto.empty()) {
            res.status = 403;
            res.set_content("HTTPS required", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Проверка admin-прав
    auto require_admin = [](httplib::Response& res) -> bool {
        if (tl_current_role != "admin") {
            res.status = 403;
            res.set_content(R"({"error":"Forbidden: admin only"})", "application/json");
            return false;
        }
        return true;
    };

    // ── GET /api/status (all roles) ──
    svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        json j = get_status_json();
        j["role"] = tl_current_role;
        res.set_content(j.dump(), "application/json");
    });

    // ── GET /api/players (all roles) ──
    svr.Get("/api/players", [this](const httplib::Request&, httplib::Response& res) {
        auto players = manager_.get_players();
        json j;
        j["online"] = players.online;
        j["max"]    = players.max;
        j["names"]  = players.names;
        res.set_content(j.dump(), "application/json");
    });

    // ── GET /api/logs (admin only) ──
    svr.Get("/api/logs", [this, require_admin](const httplib::Request&, httplib::Response& res) {
        if (!require_admin(res)) return;
        std::ifstream log;
        int attempts = 3;
        while (attempts-- > 0) {
            log.open(config_.logs_path);
            if (log.is_open()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!log.is_open()) {
            LOG_ERR("Не удалось открыть logPath: " + config_.logs_path + ", errno=" + std::to_string(errno), "WEB");
            res.status = 404;
            res.set_content(R"({"error":"Лог-файл не найден или недоступен"})", "application/json");
            return;
        }

        try {
            const size_t maxLines = static_cast<size_t>(config_.max_log_lines);
            std::deque<std::string> lastLines;
            std::string line;

            while (std::getline(log, line)) {
                line = sanitize_utf8(line);
                if (lastLines.size() >= maxLines) {
                    lastLines.pop_front();
                }
                lastLines.push_back(std::move(line));
            }

            std::string logs;
            for (const auto& l : lastLines) {
                logs += l + "\n";
            }

            json response = {{"logs", logs}};
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG_ERR("Ошибка при чтении логов: " + std::string(e.what()), "WEB");
            res.status = 500;
            res.set_content(R"({"error":"Ошибка при чтении лог-файла"})", "application/json");
        }
    });

    // ── GET /api/download-modpack (admin only) ──
    svr.Get("/api/download-modpack", [this, require_admin](const httplib::Request&, httplib::Response& res) {
        if (!require_admin(res)) return;
        if (!fs::exists(config_.modpack_path)) {
            LOG_ERR("Файл не существует: " + config_.modpack_path, "WEB");
            res.status = 404;
            res.set_content("Файл не найден", "text/plain");
            return;
        }

        auto file_stream = std::make_shared<std::ifstream>(
            config_.modpack_path, std::ios::binary | std::ios::ate);

        if (!*file_stream) {
            LOG_ERR("Не удалось открыть файл сборки: " + config_.modpack_path, "WEB");
            res.status = 500;
            res.set_content("Ошибка открытия файла", "text/plain");
            return;
        }

        auto file_size = file_stream->tellg();
        file_stream->seekg(0);

        res.set_header("Content-Type", "application/zip");
        res.set_header("Content-Disposition",
            "attachment; filename=" + fs::path(config_.modpack_path).filename().string());
        res.set_header("Content-Length", std::to_string(file_size));
        res.set_header("Cache-Control", "no-cache");

        constexpr size_t buffer_size = 64 * 1024;
        auto buffer = std::make_shared<std::vector<char>>(buffer_size);
        int upload_limit = config_.upload_limit_mb * 1024 * 1024;

        auto provider = [
            file_stream, buffer, upload_limit,
            start_time = std::chrono::steady_clock::now(),
            bytes_sent = size_t(0)
        ](size_t offset, size_t length, httplib::DataSink& sink) mutable -> bool {
            try {
                file_stream->seekg(static_cast<std::streamoff>(offset));

                while (length > 0 && !file_stream->eof()) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - start_time).count();

                    size_t max_allowed = upload_limit > 0
                        ? static_cast<size_t>(static_cast<int64_t>(upload_limit) * (elapsed + 1) / 1000) - bytes_sent
                        : (std::numeric_limits<size_t>::max)();

                    if (max_allowed <= 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }

                    auto read_size = static_cast<std::streamsize>(
                        std::min<size_t>({length, buffer->size(), max_allowed}));

                    file_stream->read(buffer->data(), read_size);
                    auto count = file_stream->gcount();
                    if (count <= 0) break;

                    if (!sink.write(buffer->data(), static_cast<size_t>(count))) {
                        return false;
                    }

                    bytes_sent += static_cast<size_t>(count);
                    length -= static_cast<size_t>(count);
                }

                if (length == 0 || file_stream->eof()) {
                    sink.done();
                    return true;
                }
            } catch (...) {
                return false;
            }
            return false;
        };

        res.set_content_provider(
            static_cast<size_t>(file_size), "application/zip", provider);
    });

    // ── POST /api/start (admin only) ──
    svr.Post("/api/start", [this, require_admin](const httplib::Request&, httplib::Response& res) {
        if (!require_admin(res)) return;
        LOG_INFO("Запрошен запуск сервера через API", "WEB");
        manager_.start();
        res.set_content(get_status_json().dump(), "application/json");
    });

    // ── POST /api/stop (admin only) ──
    svr.Post("/api/stop", [this, require_admin](const httplib::Request&, httplib::Response& res) {
        if (!require_admin(res)) return;
        LOG_INFO("Запрошена остановка сервера через API", "WEB");
        manager_.stop();
        res.set_content(get_status_json().dump(), "application/json");
    });

    // ── POST /api/command (admin only) ──
    svr.Post("/api/command", [this, require_admin](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(res)) return;
        try {
            auto body = json::parse(req.body);
            std::string cmd = body["command"].get<std::string>();

            // Валидация команды
            cmd.erase(0, cmd.find_first_not_of(" \t"));
            cmd.erase(cmd.find_last_not_of(" \t") + 1);

            if (cmd.empty()) {
                res.status = 400;
                res.set_content(json{{"error", "Пустая команда"}}.dump(), "application/json");
                return;
            }

            LOG_INFO("API команда: " + cmd, "WEB");
            manager_.send_command(cmd);
            res.set_content(get_status_json().dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json{{"error", "invalid request"}}.dump(), "application/json");
        }
    });

    // ── POST /api/kick (admin only) ──
    svr.Post("/api/kick", [this, require_admin](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(res)) return;
        try {
            auto body = json::parse(req.body);
            std::string player = body["player"].get<std::string>();
            std::string reason = body.value("reason", "Kicked by admin");

            if (player.empty() || player.size() > 16) {
                res.status = 400;
                res.set_content(R"({"error":"Invalid player name"})", "application/json");
                return;
            }

            std::string result = manager_.rcon_command("kick " + player + " " + reason);
            LOG_INFO("Kick: " + player + " (reason: " + reason + ")", "WEB");
            res.set_content(json{{"result", result}}.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json{{"error", "invalid request"}}.dump(), "application/json");
        }
    });

    // ── POST /api/ban (admin only) ──
    svr.Post("/api/ban", [this, require_admin](const httplib::Request& req, httplib::Response& res) {
        if (!require_admin(res)) return;
        try {
            auto body = json::parse(req.body);
            std::string player = body["player"].get<std::string>();
            std::string reason = body.value("reason", "Banned by admin");

            if (player.empty() || player.size() > 16) {
                res.status = 400;
                res.set_content(R"({"error":"Invalid player name"})", "application/json");
                return;
            }

            std::string result = manager_.rcon_command("ban " + player + " " + reason);
            LOG_INFO("Ban: " + player + " (reason: " + reason + ")", "WEB");
            res.set_content(json{{"result", result}}.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json{{"error", "invalid request"}}.dump(), "application/json");
        }
    });

    // ── Фронтенд ──
    svr.set_mount_point("/", config_.web_root);
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/index.html");
    });
}

// ────────────────────────────────────────────────────────────────
// run — только запуск сервера (маршруты уже зарегистрированы)
// ────────────────────────────────────────────────────────────────
void HttpServer::run() {
    if (!std::ifstream(config_.logs_path)) {
        LOG_ERR("Лог-файл не найден: " + config_.logs_path, "WEB");
    }
    if (!std::ifstream(config_.modpack_path)) {
        LOG_ERR("Файл сборки модов не найден: " + config_.modpack_path, "WEB");
    }
    if (!fs::exists(config_.web_root)) {
        LOG_ERR("Директория с сайтом не найдена: " + config_.web_root, "WEB");
    }

    LOG_INFO("HTTP сервер запущен на порту: " + std::to_string(config_.port), "WEB");
    try {
        int pool_size = config_.thread_pool_size;
        svr.new_task_queue = [pool_size] { return new httplib::ThreadPool(pool_size); };
        if (!svr.listen("0.0.0.0", config_.port)) {
            LOG_ERR("Не удалось запустить сервер!", "WEB");
        }
    } catch (const std::exception& ex) {
        LOG_ERR(std::string("Ошибка: ") + ex.what(), "WEB");
    }
}

void HttpServer::stop() {
    LOG_WARNING("Остановка WEB сервера...", "WEB");
    svr.stop();

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock != INVALID_SOCKET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = htons(static_cast<u_short>(config_.port));
        connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        closesocket(sock);
    }
#endif
    LOG_INFO("Сервер остановлен", "WEB");
}
