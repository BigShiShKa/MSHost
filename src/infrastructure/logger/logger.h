#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <memory>
#include <windows.h>

enum class LogLevel { DEBUG, INFO, WARNING, ERR, CRITICAL };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // ─────────────────────────────
    // Sink registration
    // ─────────────────────────────
    void add_sink(const std::string& name, const std::string& filepath) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto file = std::make_unique<std::ofstream>(
            filepath,
            std::ios::out | std::ios::app | std::ios::binary
        );

        if (file->is_open()) {
            write_bom(*file);
            sinks_[name] = std::move(file);
        }
    }

    void remove_sink(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.erase(name);
    }

    // ─────────────────────────────
    // Config
    // ─────────────────────────────
    void set_console_output(bool enabled) {
        consoleOutput_ = enabled;
    }

    void set_min_level(LogLevel level) {
        minLevel_ = level;
    }

    // ─────────────────────────────
    // Logging
    // ─────────────────────────────
    void log(LogLevel level,
             const std::string& message,
             const std::string& target,
             const std::string& module = "")
    {
        if (level < minLevel_) return;

        const char* levelStr =
            level == LogLevel::DEBUG ? "DEBUG" :
            level == LogLevel::INFO ? "INFO" :
            level == LogLevel::WARNING ? "WARN" :
            level == LogLevel::ERR ? "ERROR" : "CRIT";

        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << "["
            << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S")
            << "] "
            << "[" << levelStr << "] ";

        if (!target.empty())
            oss << "[" << target << "] ";

        if (!module.empty())
            oss << "[" << module << "] ";

        oss << message;

        std::string out = oss.str();

        std::lock_guard<std::mutex> lock(mutex_);

        // ─── console ───
        if (consoleOutput_) {
            write_console_utf8(out);
        }

        // ─── file sinks ───
        auto it = sinks_.find(target);
        if (it != sinks_.end() && it->second && it->second->is_open()) {
            *(it->second) << out << '\n';
            it->second->flush();
        }
    }

private:
    Logger()
        : consoleOutput_(true),
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

        int wlen = MultiByteToWideChar(
            CP_UTF8, 0,
            utf8.c_str(),
            (int)utf8.size(),
            nullptr, 0
        );

        if (wlen <= 0) return;

        std::wstring wstr(wlen, L'\0');

        MultiByteToWideChar(
            CP_UTF8, 0,
            utf8.c_str(),
            (int)utf8.size(),
            wstr.data(), wlen
        );

        wstr += L'\n';

        DWORD written;
        WriteConsoleW(hOut, wstr.c_str(), (DWORD)wstr.size(), &written, nullptr);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<std::ofstream>> sinks_;
    std::mutex mutex_;

    bool consoleOutput_;
    LogLevel minLevel_;
};

// ── macros ─────────────────────────────

#define LOG_DEBUG(msg, target, module)    Logger::instance().log(LogLevel::DEBUG, msg, target, module)
#define LOG_INFO(msg, target, module)     Logger::instance().log(LogLevel::INFO, msg, target, module)
#define LOG_WARNING(msg, target, module)  Logger::instance().log(LogLevel::WARNING, msg, target, module)
#define LOG_ERR(msg, target, module)      Logger::instance().log(LogLevel::ERR, msg, target, module)
#define LOG_CRITICAL(msg, target, module) Logger::instance().log(LogLevel::CRITICAL, msg, target, module)

#endif