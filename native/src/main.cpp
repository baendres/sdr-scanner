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
std::atomic<bool> g_restartRequested{false};
std::atomic<bool> g_audioMixerDied{false};
void handleSignal(int) { g_stop = true; }

// Exit code used only for the "Restart Now" path (never for a plain SIGINT/SIGTERM stop) - see
// the constructor comment below. Any non-zero value works; this one's arbitrary but distinct
// from 1 (the generic "fatal error" path just below) so the two are distinguishable in logs.
constexpr int kRestartRequestedExitCode = 75;
// Exit code used when Scanner detects AudioMixer's thread has died (see
// Scanner::setAudioMixerDiedCallback) - distinct from the other two for the same reason.
// Non-zero for the same purpose as kRestartRequestedExitCode: `restart: on-failure` brings the
// process back up automatically rather than leaving it silently dead with no audio output.
constexpr int kAudioMixerDiedExitCode = 76;
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

        // Lets the settings page's "Restart Now" button (POST /api/restart) request the same
        // graceful shutdown Ctrl+C/SIGTERM does, but exits non-zero afterward instead of 0 (see
        // kRestartRequestedExitCode) so it can be told apart from a deliberate stop. Paired with
        // `restart: on-failure` in native/docker-compose.yaml, that's what brings the process
        // back up with any pending receiver/output config applied: on-failure restarts on a
        // non-zero exit but - unlike unless-stopped/always - never auto-starts the container
        // just because the host rebooted, so this won't fight with another project's containers
        // that are meant to be the ones that come back after a reboot. Running the binary
        // directly with no restart policy means the button just stops the process.
        sdrscan::HttpServer httpServer(scanner, settings.httpHost, settings.httpPort, webRoot,
                                        []() { g_restartRequested = true; g_stop = true; });

        // Port of Scanner.py's audioServerProcess.is_alive() watchdog - see
        // Scanner::setAudioMixerDiedCallback's header comment. A dead audio pipeline with
        // everything else (receivers, HTTP server) still running is a confusing, silently
        // broken state - exiting non-zero here lets the deployment's restart policy recover it
        // the same way the "Restart Now" button does, rather than leaving it stuck.
        scanner.setAudioMixerDiedCallback([]() { g_audioMixerDied = true; g_stop = true; });

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

    if (g_audioMixerDied) return kAudioMixerDiedExitCode;
    return g_restartRequested ? kRestartRequestedExitCode : 0;
}
