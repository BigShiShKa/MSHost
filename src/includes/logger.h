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

    void init(bool consoleOutput = true,
              const std::string& filename    = "server.log",
              const std::string& webFilename = "web.log")
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now      = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        {
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d_%H-%M-%S");
            sessionDirName_ = oss.str();
        }

        auto open_file = [](const std::string& path, std::ofstream& f) {
            if (path.empty()) return false;
            f.open(path, std::ios::out | std::ios::app);
            return f.is_open();
        };

        fileOutput_ = open_file(filename, logFile_);
        webOutput_  = open_file(webFilename, webFile_);
        consoleOutput_ = consoleOutput;
    }

    void setMinLevel(LogLevel level) { minLevel_ = level; }

    void log(LogLevel level, const std::string& message, const std::string& module = "") {
        if (level < minLevel_) return;

        std::string cleaned = message;
        if (module == "MC_OUT") {
            cleaned = strip_mc_timestamp(message);
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
            write_console_utf8(out);
        }

        bool isWeb = (module == "WEB" || module == "HTTP" || module == "API");
        if (fileOutput_ && !isWeb) logFile_ << out << std::endl;
        if (webOutput_  && isWeb)  webFile_ << out << std::endl;
    }

    void finalize() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            if (archived_) return;
            archived_ = true;

            if (logFile_.is_open()) logFile_.close();
            if (webFile_.is_open()) webFile_.close();

            fs::path logsRoot = "./logs";
            if (!fs::exists(logsRoot)) fs::create_directory(logsRoot);

            fs::path sessionDir = logsRoot / sessionDirName_;
            if (!fs::exists(sessionDir)) fs::create_directory(sessionDir);

            auto safe_copy_and_overwrite = [&](const char* src, const char* latest) {
                fs::path srcPath{src};
                if (!fs::exists(srcPath)) return;

                fs::path latestPath = logsRoot / latest;
                std::error_code ec;
                fs::remove(latestPath, ec);
                fs::copy_file(srcPath, sessionDir / srcPath.filename(), fs::copy_options::overwrite_existing, ec);
                fs::copy_file(srcPath, latestPath, fs::copy_options::overwrite_existing, ec);
                fs::remove(srcPath, ec);
            };

            if (fileOutput_) safe_copy_and_overwrite("server.log", "latest_server.log");
            if (webOutput_)  safe_copy_and_overwrite("web.log",    "latest_web.log");
        } catch (const std::exception& e) {
            std::cerr << "[LOGGER] finalize error: " << e.what() << std::endl;
        }
    }

    ~Logger() { try { finalize(); } catch (...) {} }

private:
    Logger() : consoleOutput_(true), fileOutput_(false), webOutput_(false),
               minLevel_(LogLevel::INFO), archived_(false) {}

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    // Убираем MC-таймстемп вида [HH:MM:SS] без regex
    static std::string strip_mc_timestamp(const std::string& msg) {
        // Формат: [HH:MM:SS] текст...
        if (msg.size() >= 11 && msg[0] == '[' &&
            msg[3] == ':' && msg[6] == ':' && msg[9] == ']')
        {
            size_t start = 10;
            while (start < msg.size() && msg[start] == ' ') ++start;
            return msg.substr(start);
        }
        return msg;
    }

    // Вывод UTF-8 в Windows-консоль через WriteConsoleW
    static void write_console_utf8(const std::string& utf8) {
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE) return;

        // UTF-8 → wide
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0) return;

        std::wstring wstr(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                            static_cast<int>(utf8.size()), wstr.data(), wlen);
        wstr += L'\n';

        DWORD written;
        WriteConsoleW(hOut, wstr.c_str(), static_cast<DWORD>(wstr.size()), &written, nullptr);
#else
        std::cout << utf8 << '\n';
#endif
    }

    std::ofstream logFile_;
    std::ofstream webFile_;

    std::mutex mutex_;
    bool consoleOutput_;
    bool fileOutput_;
    bool webOutput_;
    bool archived_;
    LogLevel minLevel_;

    std::string sessionDirName_;
};

// ── Макросы ─────────────────────────────────────────────────────────────
#define LOG_DEBUG(msg, module)    Logger::instance().log(LogLevel::DEBUG,    msg, module)
#define LOG_INFO(msg, module)     Logger::instance().log(LogLevel::INFO,     msg, module)
#define LOG_WARNING(msg, module)  Logger::instance().log(LogLevel::WARNING,  msg, module)
#define LOG_ERR(msg, module)      Logger::instance().log(LogLevel::ERR,      msg, module)
#define LOG_CRITICAL(msg, module) Logger::instance().log(LogLevel::CRITICAL, msg, module)

#endif // LOGGER_H
