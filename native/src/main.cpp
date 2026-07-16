#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "db/Database.h"
#include "scanner/Scanner.h"
#include "control/HttpServer.h"

namespace {
std::atomic<bool> g_stop{false};
void handleSignal(int) { g_stop = true; }
} // namespace

int main(int argc, char** argv) {
    std::string dbPath = "sdrscan.db";
    std::string webRoot = "web";
    std::string host;
    int port = -1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--db") && i + 1 < argc) {
            dbPath = argv[++i];
        } else if ((arg == "-w" || arg == "--web-root") && i + 1 < argc) {
            webRoot = argv[++i];
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [-d sdrscan.db] [-w web/] [--host 0.0.0.0] [--port 8080]\n";
            return 0;
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    try {
        sdrscan::Database db(dbPath);
        db.initSchema();

        sdrscan::Scanner scanner(db);
        scanner.loadConfigFromDatabase();

        auto settings = db.loadScannerSettings();
        if (!host.empty()) settings.httpHost = host;
        if (port > 0) settings.httpPort = port;

        sdrscan::HttpServer httpServer(scanner, settings.httpHost, settings.httpPort, webRoot);

        scanner.start();
        httpServer.start();

        std::cout << "sdr-scanner (native) running - Ctrl+C to stop\n";
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::cout << "Shutting down...\n";
        httpServer.stop();
        scanner.stop();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
