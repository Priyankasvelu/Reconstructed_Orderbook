#pragma once

#include <unordered_map>
#include <map>
#include <string>
#include <iostream>
#include <cstdint>

// DBN Record - Normalized market data record
struct DBNRecord {
    std::uint64_t order_id;
    std::int64_t price;
    std::int32_t size;
    char side;        // 'B' (Bid) or 'A' (Ask)
    char action;      // 'A' (Add), 'M' (Modify), 'C' (Cancel), 'F' (Fill)
};

// Order Node - Represents a single order in the book
struct OrderNode {
    std::uint64_t order_id;
    std::int64_t price;
    std::int32_t size;
    char side;        // 'B' or 'A'
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;
};

// Price Level - All orders at a single price point
struct PriceLevel {
    std::int32_t total_size = 0;
    OrderNode* head = nullptr;
    OrderNode* tail = nullptr;
};

// Order Book Change - Result of applying an update
struct OrderBookChange {
    char action;                    // 'A', 'M', 'C', 'F'
    std::int64_t best_bid;
    std::int64_t best_ask;
    std::int32_t bid_size;
    std::int32_t ask_size;
};

// Order Book Class
class OrderBook {
private:
    // O(1) Lookup: Maps Order ID to its corresponding OrderNode pointer
    std::unordered_map<std::uint64_t, OrderNode*> order_map_;

    // Ordered containers for Best Bid/Offer (BBO) lookup
    // Bids: Sorted descending by price (highest bid first)
    std::map<std::int64_t, PriceLevel, std::greater<std::int64_t>> bids_;
    // Asks: Sorted ascending by price (lowest ask first)
    std::map<std::int64_t, PriceLevel> asks_;

    // Simple memory pool (pre-allocated OrderNodes)
    static constexpr size_t MAX_ORDERS = 10000;
    OrderNode node_pool_[MAX_ORDERS];
    size_t next_free_index_ = 0;
    OrderNode* free_list_head_ = nullptr;

    // Private helper functions for O(1) list manipulation
    void insert_order_into_level(PriceLevel& level, OrderNode* node);
    void remove_order_from_level(PriceLevel& level, OrderNode* node);
    
    OrderNode* allocate_node();
    void deallocate_node(OrderNode* node);

public:
    OrderBook();
    ~OrderBook() = default;

    // Apply update using DBNRecord
    OrderBookChange apply_update(const DBNRecord& record);

    // Get current best bid and ask
    std::pair<std::int64_t, std::int32_t> get_best_bid() const;
    std::pair<std::int64_t, std::int32_t> get_best_ask() const;

    // Print the order book state
    void print_book() const;

    // Get book snapshot
    OrderBookChange snapshot_top_of_book() const;

    
    std::string to_json(bool pretty = true) const;
    void save_json(const std::string& path, bool pretty = true) const;
};
