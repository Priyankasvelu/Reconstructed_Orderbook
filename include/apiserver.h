#pragma once

#include <string>
#include <atomic>
#include <memory>
#include "engine.h"
#include "metrics.h"

//  HTTP/WebSocket server exposing order book and metrics
class ApiServer {
public:
    ApiServer(Engine* engine, int port = 8080);
    ~ApiServer();
    
    void start();  // Blocking call; run in separate thread
    void stop();
    
    int get_connected_clients() const { return connected_clients_.load(); }
    
private:
    Engine* engine_;
    int port_;
    std::atomic<int> connected_clients_{0};
    std::atomic<int> peak_connected_clients_{0};
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> total_events_streamed_{0};
    std::atomic<bool> running_{false};
    std::unique_ptr<httplib::Server> server_; 
    // Handlers
    std::string handle_orderbook();
    std::string handle_metrics();
};
