#pragma once

#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <string>
#include <mutex>

//  metrics collector for latency and throughput
struct Metrics {
    // Counters
    std::atomic<uint64_t> total_messages{0};
    std::atomic<uint64_t> decode_errors{0};      // malformed or failed record decode
    std::atomic<uint64_t> replay_errors{0};      // exceptions during replay loop
    uint64_t replay_duration_ns = 0; // total elapsed time for replay

    // Last error message 
    void set_last_error(const std::string& msg);
    std::string last_error() const;

    // Latency recording
    void record_latency(uint64_t ns);
    double p50() const;
    double p95() const;
    double p99() const;

    double throughput_msg_per_sec() const {
        if (replay_duration_ns == 0) return 0.0;
        return static_cast<double>(total_messages.load()) / (replay_duration_ns / 1e9);
    }

    bool p99_exceeds(uint64_t threshold_ns) const { return p99() > static_cast<double>(threshold_ns); }

private:
    mutable std::vector<uint64_t> latencies_;
    mutable std::mutex latency_mutex_;
    mutable std::mutex error_mutex_;
    mutable std::string last_error_message_;
};
