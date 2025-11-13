#pragma once

#include <string>
#include <memory>
#include <cstddef>
#include "orderbook.h"
#include "logger.h"
#include "metrics.h"

#ifdef __has_include
#  if __has_include(<databento/record.hpp>)
#    include <databento/record.hpp>
#    include <databento/enums.hpp>
#    include <databento/dbn_file_store.hpp>
#    define HFT_HAS_DATABENTO 1
#  endif
#endif

class Engine {
public:
    explicit Engine(std::string dbn_path = "");
    void set_dbn_path(const std::string& path) { dbn_path_ = path; }
    void init();
    void request_stop() { running_.store(false, std::memory_order_relaxed); }
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Replay a DBN file if Databento headers are available
    void replay(const AsyncLogger& logger, std::size_t max_snapshots = 20);
    // Reconstruct full order book across publishers and output JSON summary.
    // levels parameter controls how many price levels per side to include for each publisher book.
    std::string reconstruct_orderbook_json(std::size_t levels = 5) const;
    void save_aggregated_orderbook_json(const std::string& path, std::size_t levels = 5) const;

    // Access JSON representation of current book
    std::string orderbook_json(bool pretty = true) const { return book_.to_json(pretty); }
    void save_book_json(const std::string& path, bool pretty = true) const { book_.save_json(path, pretty); }

    // Access performance metrics
    const Metrics& get_metrics() const { return metrics_; }

private:
    std::string dbn_path_;
    OrderBook book_{}; // uses default constructor
    mutable Metrics metrics_{}; // mutable for const reconstruct_orderbook_json
    mutable std::atomic<bool> running_{true};

#ifdef HFT_HAS_DATABENTO
    DBNRecord map_mbo(const databento::MboMsg& mbo) const;
#endif
};
