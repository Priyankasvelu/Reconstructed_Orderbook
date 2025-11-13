// Main driver for HFT engine: DBN replay only (Day 1 synthetic tests removed)
#include "include/engine.h"
#include "include/logger.h"
#include "include/apiserver.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <csignal>

// Global pointers for signal handlers (simple approach)
static Engine* g_engine_ptr = nullptr;
static ApiServer* g_api_ptr = nullptr;
static std::atomic<bool> g_shutdown{false};

int main(int argc, char* argv[]) {
    AsyncLogger logger; // simple logger

    // Determine dbn path: precedence ENV(DBN_FILE) > CLI arg > autodiscover.
    std::string dbn_path;
    if (const char* env_dbn = std::getenv("DBN_FILE")) {
        dbn_path = env_dbn;
    } else if (argc > 1) {
        dbn_path = argv[1];
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(".")) {
            if (entry.path().extension() == ".dbn") { dbn_path = entry.path().string(); break; }
        }
    }

    if (dbn_path.empty()) {
        std::cerr << "No .dbn file provided or found in project root." << std::endl;
        std::cerr << "Usage: " << argv[0] << " <file.dbn>" << std::endl;
        return 1;
    }

    Engine engine(dbn_path);
    engine.init();
    g_engine_ptr = &engine;
    
    // Determine server port: ENV(PORT) override
    int port = 8080;
    if (const char* env_port = std::getenv("PORT")) {
        try { port = std::stoi(env_port); } catch (...) { /* ignore */ }
    }
    // Start API server in background thread
    ApiServer api_server(&engine, port);
    g_api_ptr = &api_server;
    std::thread api_thread([&api_server, port]() {
        std::cout << "API server starting on http://localhost:" << port << "\n";
        api_server.start();
    });

    // Graceful shutdown via signals: SIGINT/SIGTERM
    auto signal_handler = [](int sig){
        if (g_shutdown.load()) return; // already handling
        g_shutdown.store(true);
        if (g_engine_ptr) g_engine_ptr->request_stop();
        if (g_api_ptr) g_api_ptr->stop();
        std::cerr << "\nSignal " << sig << " received. Initiating graceful shutdown..." << std::endl;
    };
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Thread 1: Replay DBN and build aggregated book (blocking)
    std::cout << "Replaying DBN file...\n";
    engine.save_aggregated_orderbook_json("aggregated_orderbook.json", 0); // 0 = no limit: include all available levels (single pass replay)
    
    // Optional performance metrics printing (disabled by default when QUIET_METRICS=1)
    bool show_metrics = true;
    if (const char* quiet = std::getenv("QUIET_METRICS")) {
        if (std::string(quiet) == "1") show_metrics = false;
    }
    if (show_metrics) {
        const Metrics& m = engine.get_metrics();
        std::cout << "\n=== Performance Metrics ===\n";
        std::cout << "total_messages: " << m.total_messages.load() << "\n";
        std::cout << "throughput: " << (m.throughput_msg_per_sec() / 1000.0) << " K msg/sec\n";
        std::cout << "p50 latency: " << (m.p50() / 1000.0) << " µs\n";
        std::cout << "p95 latency: " << (m.p95() / 1000.0) << " µs\n";
        std::cout << "p99 latency: " << (m.p99() / 1000.0) << " µs\n";
        uint64_t latency_warn_threshold_ns = 10000000; // 10 ms default
        if (const char* envp = std::getenv("LATENCY_P99_WARN_NS")) {
            try { latency_warn_threshold_ns = static_cast<uint64_t>(std::stoull(envp)); } catch (...) {}
        }
        if (m.p99() > static_cast<double>(latency_warn_threshold_ns)) {
            std::cerr << "[WARN] p99 latency " << m.p99() << " ns exceeded threshold " << latency_warn_threshold_ns << " ns" << std::endl;
        }
    }
    
    std::cout << "\nAPI server running. Test with:\n";
    std::cout << "  curl http://localhost:" << port << "/orderbook\n";
    std::cout << "  curl http://localhost:" << port << "/metrics\n";
    std::cout << "Press Ctrl+C to exit.\n";
    
    // Keep main thread alive for API server until signal triggers stop
    api_thread.join();
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
