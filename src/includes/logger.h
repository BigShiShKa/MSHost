#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>
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
              const std::string& filename = "mc.log",
              const std::string& webFilename = "web.log")
    {
        std::lock_guard<std::mutex> lock(mutex_);

        consoleOutput_ = consoleOutput;
        archived_ = false;

        // ─── session folder ───
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d_%H-%M-%S");
        sessionDirName_ = oss.str();

        fs::create_directories("./logs");
        fs::create_directories("./logs/" + sessionDirName_);

        // ─── open files ───
        logFile_.open(filename, std::ios::out | std::ios::trunc | std::ios::binary);
        webFile_.open(webFilename, std::ios::out | std::ios::trunc | std::ios::binary);

        if (logFile_.is_open()) write_bom(logFile_);
        if (webFile_.is_open()) write_bom(webFile_);
    }

    void setMinLevel(LogLevel level) {
        minLevel_ = level;
    }

    void log(LogLevel level, const std::string& message, const std::string& module = "") {
        if (level < minLevel_) return;

        const char* levelStr =
            level == LogLevel::DEBUG ? "DEBUG" :
            level == LogLevel::INFO ? "INFO" :
            level == LogLevel::WARNING ? "WARN" :
            level == LogLevel::ERR ? "ERROR" : "CRIT";

        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << "[" << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S") << "] "
            << "[" << levelStr << "] ";

        if (!module.empty())
            oss << "[" << module << "] ";

        oss << message;

        std::string out = oss.str();

        std::lock_guard<std::mutex> lock(mutex_);

        // ─── console ───
        if (consoleOutput_) {
            write_console_utf8(out);
        }

        // ─── file ───
        if (fileOutput_ && logFile_.is_open()) {
            logFile_ << out << '\n';
            logFile_.flush();
        }

        // ─── web log routing ───
        if (webOutput_ && webFile_.is_open()) {
            if (module == "WEB" || module == "HTTP" || module == "API") {
                webFile_ << out << '\n';
                webFile_.flush();
            }
        }
    }

    void finalize() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (archived_) return;
        archived_ = true;

        if (logFile_.is_open()) logFile_.close();
        if (webFile_.is_open()) webFile_.close();

        fs::path logsRoot = "./logs";
        fs::path sessionDir = logsRoot / sessionDirName_;

        fs::create_directories(sessionDir);

        auto copy = [&](const std::string& file) {
            fs::path src(file);
            if (!fs::exists(src)) return;

            fs::copy_file(src, sessionDir / src.filename(),
                          fs::copy_options::overwrite_existing);
        };

        copy("mc.log");
        copy("web.log");
    }

    ~Logger() {
        try { finalize(); } catch (...) {}
    }

private:
    Logger()
        : consoleOutput_(true),
          fileOutput_(true),
          webOutput_(true),
          archived_(false),
          minLevel_(LogLevel::INFO) {}

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // ─────────────────────────────
    // UTF-8 BOM
    // ─────────────────────────────
    static void write_bom(std::ofstream& f) {
        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        f.write(reinterpret_cast<const char*>(bom), 3);
    }

    // ─────────────────────────────
    // UTF-8 → WinAPI console
    // ─────────────────────────────
    static void write_console_utf8(const std::string& utf8) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE) return;

        int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                       utf8.c_str(),
                                       (int)utf8.size(),
                                       nullptr, 0);

        if (wlen <= 0) return;

        std::wstring wstr(wlen, L'\0');

        MultiByteToWideChar(CP_UTF8, 0,
                            utf8.c_str(),
                            (int)utf8.size(),
                            wstr.data(), wlen);

        wstr += L'\n';

        DWORD written;
        WriteConsoleW(hOut, wstr.c_str(), (DWORD)wstr.size(), &written, nullptr);
    }

private:
    std::ofstream logFile_;
    std::ofstream webFile_;

    std::mutex mutex_;

    bool consoleOutput_;
    bool fileOutput_ = true;
    bool webOutput_ = true;
    bool archived_;

    LogLevel minLevel_;

    std::string sessionDirName_;
};

// ── macros ─────────────────────────────

#define LOG_DEBUG(msg, module)    Logger::instance().log(LogLevel::DEBUG, msg, module)
#define LOG_INFO(msg, module)     Logger::instance().log(LogLevel::INFO, msg, module)
#define LOG_WARNING(msg, module)  Logger::instance().log(LogLevel::WARNING, msg, module)
#define LOG_ERR(msg, module)      Logger::instance().log(LogLevel::ERR, msg, module)
#define LOG_CRITICAL(msg, module) Logger::instance().log(LogLevel::CRITICAL, msg, module)

#endif