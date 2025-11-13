#include "../include/apiserver.h"
#include <httplib.h>
#include <sstream>
#include <iomanip>
#include <thread>

ApiServer::ApiServer(Engine* engine, int port) 
    : engine_(engine), port_(port) {}

ApiServer::~ApiServer() {
    stop();
}

std::string ApiServer::handle_orderbook() {
    // Return aggregated order book JSON snapshot (all levels)
    return engine_->reconstruct_orderbook_json(0);
}

std::string ApiServer::handle_metrics() {
    const Metrics& m = engine_->get_metrics();
    std::ostringstream oss;
    // Threshold for latency spike (nanoseconds). Can be overridden via env LATENCY_P99_THRESHOLD_NS
    uint64_t threshold_ns = 10000000; // 10 ms default
    if (const char* envp = std::getenv("LATENCY_P99_THRESHOLD_NS")) {
        try { threshold_ns = static_cast<uint64_t>(std::stoull(envp)); } catch (...) {}
    }
    bool spike = m.p99_exceeds(threshold_ns);
    oss << "{\n"
        << "  \"connected_clients\": " << connected_clients_.load() << ",\n"
        << "  \"peak_concurrent_clients\": " << peak_connected_clients_.load() << ",\n"
        << "  \"total_connections\": " << total_connections_.load() << ",\n"
        << "  \"total_events_streamed\": " << total_events_streamed_.load() << ",\n"
        << "  \"total_messages\": " << m.total_messages.load() << ",\n"
        << "  \"replay_errors\": " << m.replay_errors.load() << ",\n"
        << "  \"decode_errors\": " << m.decode_errors.load() << ",\n"
        << "  \"latency_ns_p50\": " << m.p50() << ",\n"
        << "  \"latency_ns_p95\": " << m.p95() << ",\n"
        << "  \"latency_ns_p99\": " << m.p99() << ",\n"
        << "  \"throughput_msg_per_sec\": " << std::fixed << std::setprecision(2) << m.throughput_msg_per_sec() << ",\n"
        << "  \"p99_threshold_ns\": " << threshold_ns << ",\n"
        << "  \"latency_spike\": " << (spike ? "true" : "false") << ",\n"
        << "  \"last_error\": \"" << m.last_error() << "\"\n"
        << "}\n";
    return oss.str();
}

void ApiServer::start() {
    running_ = true;
    server_ = std::make_unique<httplib::Server>();
    auto& svr = *server_;
    
    // GET /orderbook - return current aggregated book snapshot as JSON
    svr.Get("/orderbook", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_orderbook(), "application/json");
    });
    
    // GET /metrics - return performance metrics
    svr.Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(handle_metrics(), "application/json");
    });
    
    // SSE stream endpoint for continuous order book updates
    svr.Get("/stream", [this](const httplib::Request&, httplib::Response& res) {
        // Track connection
        int current = connected_clients_.fetch_add(1, std::memory_order_relaxed) + 1;
        total_connections_.fetch_add(1, std::memory_order_relaxed);
        
        // Update peak if current exceeds it
        int peak = peak_connected_clients_.load(std::memory_order_relaxed);
        while (current > peak && !peak_connected_clients_.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
            // Retry if another thread updated peak
        }
        
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider("text/event-stream",
            [this](size_t /*offset*/, httplib::DataSink& sink) {
                if (!running_.load(std::memory_order_relaxed) || !engine_->is_running()) return false;
                // Build one SSE event
                std::string payload = handle_orderbook();
                std::string event = "data: " + payload + "\n\n";
                sink.write(event.c_str(), event.size());
                total_events_streamed_.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 5 updates/sec
                return true; // continue streaming
            },
            [this](bool) { // done callback
                connected_clients_.fetch_sub(1, std::memory_order_relaxed);
            }
        );
    });
    
    std::cout << "API server listening on http://0.0.0.0:" << port_ << "\n";
    svr.listen("0.0.0.0", port_);
}

void ApiServer::stop() {
    running_ = false;
    if (server_) server_->stop();
}
