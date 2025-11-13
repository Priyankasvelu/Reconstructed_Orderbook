#include "../include/metrics.h"

void Metrics::record_latency(uint64_t ns) {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    latencies_.push_back(ns);
}

void Metrics::set_last_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_message_ = msg;
}

std::string Metrics::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_message_;
}

double Metrics::p50() const {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (latencies_.empty()) return 0.0;
    std::vector<uint64_t> copy = latencies_;
    std::sort(copy.begin(), copy.end());
    return static_cast<double>(copy[copy.size() / 2]);
}

double Metrics::p95() const {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (latencies_.empty()) return 0.0;
    std::vector<uint64_t> copy = latencies_;
    std::sort(copy.begin(), copy.end());
    size_t idx = static_cast<size_t>(copy.size() * 0.95);
    if (idx >= copy.size()) idx = copy.size() - 1;
    return static_cast<double>(copy[idx]);
}

double Metrics::p99() const {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (latencies_.empty()) return 0.0;
    std::vector<uint64_t> copy = latencies_;
    std::sort(copy.begin(), copy.end());
    size_t idx = static_cast<size_t>(copy.size() * 0.99);
    if (idx >= copy.size()) idx = copy.size() - 1;
    return static_cast<double>(copy[idx]);
}
