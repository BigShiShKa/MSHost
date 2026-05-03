#include "./core/application.h"
#include "./utils/console.h"
#include "./workers/mc_worker.h"
#include "./workers/web_worker.h"
#include "./includes/logger.h"

#include <string>
#include <iostream>

// ─────────────────────────────
// main
// ─────────────────────────────
int main(int argc, char* argv[]) {
    try {
        setup_console();

        Application app(argc, argv);
        return app.run();

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