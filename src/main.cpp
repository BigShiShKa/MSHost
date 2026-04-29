#include "./core/application.h"
#include "./utils/console.h"
#include "./workers/mc_worker.h"
#include "./workers/web_worker.h"
#include "./includes/logger.h"

#include <string>
#include <iostream>

// ─────────────────────────────
// Mode parser
// ─────────────────────────────
std::string parse_mode(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.rfind("--mode=", 0) == 0) {
            return arg.substr(7); // после "--mode="
        }
    }

    return "core";
}

// ─────────────────────────────
// main
// ─────────────────────────────
int main(int argc, char* argv[]) {
    try {
        setup_console();

        std::string mode = parse_mode(argc, argv);

        if (mode == "core") {
            Application app(argc, argv);
            return app.run();
        }

        if (mode == "mc_worker") {
            return run_mc_worker(argc, argv);
        }

        if (mode == "web_worker") {
            return run_web_worker(argc, argv);
        }

        std::cerr << "Unknown mode\n";
        return 1;
    }
    catch (const std::exception& e) {
        Logger::instance().log(LogLevel::CRITICAL,
            std::string("UNHANDLED EXCEPTION: ") + e.what(),
            "CORE");

        Logger::instance().finalize();
        return 1;
    }
    catch (...) {
        Logger::instance().log(LogLevel::CRITICAL,
            "UNKNOWN FATAL ERROR",
            "CORE");

        Logger::instance().finalize();
        return 1;
    }
}