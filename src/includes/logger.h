#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <atomic>
#include <sstream>
#include <regex>
#include <codecvt>
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;

enum class LogLevel { DEBUG, INFO, WARNING, ERR, CRITICAL };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // ────────────────────────────────────────────────────────────────────
    //  init — открыть/подготовить логи.  Вызывать один раз в main()
    // ────────────────────────────────────────────────────────────────────
    void init(bool              consoleOutput  = true,
              const std::string& filename      = "server.log",
              const std::string& webFilename   = "web.log")
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Сгенерировать имя сессии — YYYY‑MM‑DD_HH‑MM‑SS
        auto now      = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        {
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d_%H-%M-%S");
            sessionDirName_ = oss.str();
        }

        auto open_utf8 = [](const std::string& path, std::ofstream& f) {
            if (path.empty()) return false;
            f.open(path, std::ios::out | std::ios::app);
    #ifdef _WIN32
            if (f.is_open()) {
                f.imbue(std::locale(std::locale(), new std::codecvt_utf8<char16_t>));
            }
    #endif
            return f.is_open();
        };

        fileOutput_ = open_utf8(filename, logFile_);
        webOutput_  = open_utf8(webFilename, webFile_);
        consoleOutput_ = consoleOutput;
    }

    void setMinLevel(LogLevel level) { minLevel_ = level; }

    // ────────────────────────────────────────────────────────────────────
    //  log — вывод строки
    // ────────────────────────────────────────────────────────────────────
    void log(LogLevel level, const std::string& message, const std::string& module = "") {
        if (level < minLevel_) return;

        std::string cleaned = message;
        if (module == "MC_OUT") {
            static const std::regex ts_re(R"(^\[\d{2}:\d{2}:\d{2}\]\s*)");
            cleaned = std::regex_replace(message, ts_re, "");
        }

        const char* levelStr =
            level == LogLevel::DEBUG    ? "DEBUG"  :
            level == LogLevel::INFO     ? "INFO"   :
            level == LogLevel::WARNING  ? "WARN"   :
            level == LogLevel::ERR      ? "ERROR"  : "CRIT";

        auto now      = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << "[" << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S") << "] "
            << "[" << levelStr << "] "
            << (module.empty() ? "" : "[" + module + "] ")
            << cleaned;

        std::string out = oss.str();

        std::lock_guard<std::mutex> guard(mutex_);
        if (consoleOutput_) {
    #ifdef _WIN32
            DWORD written;
            WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
            WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
    #else
            std::cout << out << '\n';
    #endif
        }
        bool isWeb = (module == "WEB" || module == "HTTP" || module == "API");
        if (fileOutput_ && !isWeb)            logFile_ << out << std::endl; // server.log ← без веба
        if (webOutput_ && isWeb)              webFile_ << out << std::endl; // web.log   ← только веб
    }

    // ────────────────────────────────────────────────────────────────────
    //  finalize — вызывается в деструкторе: архивирует логи
    // ────────────────────────────────────────────────────────────────────
    void finalize() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            if (archived_) return; // защита от двойного вызова
            archived_ = true;

            // Закрываем стримы, чтобы всё сбросить на диск
            if (logFile_.is_open()) logFile_.close();
            if (webFile_.is_open()) webFile_.close();

            fs::path logsRoot = "./logs";
            if (!fs::exists(logsRoot)) fs::create_directory(logsRoot);

            fs::path sessionDir = logsRoot / sessionDirName_;
            if (!fs::exists(sessionDir)) fs::create_directory(sessionDir);

            auto safe_copy_and_overwrite = [&](const char* src, const char* latest){
                fs::path srcPath{src};
                if(!fs::exists(srcPath)) return;

                // удалить старый latest_*, если он есть (требование пользователя)
                fs::path latestPath = logsRoot / latest;
                std::error_code ec;
                fs::remove(latestPath, ec);

                // копируем в сессию и как latest
                fs::copy_file(srcPath, sessionDir / srcPath.filename(), fs::copy_options::overwrite_existing, ec);
                fs::copy_file(srcPath, latestPath,                       fs::copy_options::overwrite_existing, ec);

                fs::remove(srcPath, ec); // удаляем из корня
            };

            if (fileOutput_) safe_copy_and_overwrite("server.log", "latest_server.log");
            if (webOutput_ ) safe_copy_and_overwrite("web.log",    "latest_web.log");
        } catch (const std::exception& e) {
            // Если архивирование сломалось — просто вывесим это в консоль
            std::cerr << "[LOGGER] finalize error: " << e.what() << std::endl;
        }
    }

    ~Logger() { try { finalize(); } catch (...) { /* ничего, мы уже уходим */ } }

private:
    Logger() : consoleOutput_(true), fileOutput_(false), webOutput_(false),
               minLevel_(LogLevel::INFO), archived_(false) {}

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream logFile_;   // server.log
    std::ofstream webFile_;   // web.log

    std::mutex mutex_;
    bool consoleOutput_;
    bool fileOutput_;
    bool webOutput_;
    bool archived_;
    LogLevel minLevel_;

    std::string sessionDirName_; // YYYY‑MM‑DD_HH‑MM‑SS
};

// ── Макросы ─────────────────────────────────────────────────────────────
#define LOG_DEBUG(msg, module)    Logger::instance().log(LogLevel::DEBUG,    msg, module)
#define LOG_INFO(msg, module)     Logger::instance().log(LogLevel::INFO,     msg, module)
#define LOG_WARNING(msg, module)  Logger::instance().log(LogLevel::WARNING,  msg, module)
#define LOG_ERR(msg, module)      Logger::instance().log(LogLevel::ERR,      msg, module)
#define LOG_CRITICAL(msg, module) Logger::instance().log(LogLevel::CRITICAL, msg, module)

#endif // LOGGER_H