#include "../include/orderbook.h"
#include <iostream>
#include <iomanip>
#include <fstream> // for std::ofstream used in save_json

OrderBook::OrderBook() {
    // Initialize free list - all nodes start as free
    for (size_t i = 0; i < MAX_ORDERS - 1; ++i) {
        node_pool_[i].next = &node_pool_[i + 1];
    }
    free_list_head_ = &node_pool_[0];
}

OrderNode* OrderBook::allocate_node() {
    if (free_list_head_ == nullptr) {
        throw std::runtime_error("Order pool exhausted!");
    }
    
    OrderNode* node = free_list_head_;
    free_list_head_ = free_list_head_->next;
    node->next = nullptr;
    node->prev = nullptr;
    return node;
}

void OrderBook::deallocate_node(OrderNode* node) {
    node->next = free_list_head_;
    free_list_head_ = node;
}

void OrderBook::insert_order_into_level(PriceLevel& level, OrderNode* node) {
    if (level.tail == nullptr) {
        level.head = level.tail = node;
        node->prev = nullptr;
        node->next = nullptr;
    } else {
        level.tail->next = node;
        node->prev = level.tail;
        node->next = nullptr;
        level.tail = node;
    }
    level.total_size += node->size;
}

void OrderBook::remove_order_from_level(PriceLevel& level, OrderNode* node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        level.head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        level.tail = node->prev;
    }

    level.total_size -= node->size;
}

OrderBookChange OrderBook::apply_update(const DBNRecord& record) {
    OrderBookChange change;
    change.action = record.action;

    switch (record.action) {
        case 'A': {  // Add
            auto node = allocate_node();
            node->order_id = record.order_id;
            node->price = record.price;
            node->size = record.size;
            node->side = record.side;

            order_map_[record.order_id] = node;

            if (record.side == 'B') {
                insert_order_into_level(bids_[record.price], node);
            } else {
                insert_order_into_level(asks_[record.price], node);
            }
            break;
        }

        case 'M': {  // Modify
            auto it = order_map_.find(record.order_id);
            if (it != order_map_.end()) {
                OrderNode* node = it->second;
                
                // Remove from old price level
                if (node->side == 'B') {
                    auto& level = bids_[node->price];
                    remove_order_from_level(level, node);
                    if (level.total_size == 0) {
                        bids_.erase(node->price);
                    }
                } else {
                    auto& level = asks_[node->price];
                    remove_order_from_level(level, node);
                    if (level.total_size == 0) {
                        asks_.erase(node->price);
                    }
                }

                // Update node
                node->price = record.price;
                node->size = record.size;

                // Add to new price level
                if (node->side == 'B') {
                    insert_order_into_level(bids_[record.price], node);
                } else {
                    insert_order_into_level(asks_[record.price], node);
                }
            }
            break;
        }

        case 'C': {  // Cancel
            auto it = order_map_.find(record.order_id);
            if (it != order_map_.end()) {
                OrderNode* node = it->second;

                if (node->side == 'B') {
                    auto& level = bids_[node->price];
                    remove_order_from_level(level, node);
                    if (level.total_size == 0) {
                        bids_.erase(node->price);
                    }
                } else {
                    auto& level = asks_[node->price];
                    remove_order_from_level(level, node);
                    if (level.total_size == 0) {
                        asks_.erase(node->price);
                    }
                }

                deallocate_node(node);
                order_map_.erase(record.order_id);
            }
            break;
        }

        case 'F': {  // Fill
            auto it = order_map_.find(record.order_id);
            if (it != order_map_.end()) {
                OrderNode* node = it->second;

                if (node->side == 'B') {
                    auto& level = bids_[node->price];
                    remove_order_from_level(level, node);
                    if (level.total_size == 0) {
                        bids_.erase(node->price);
                    }
                } else {
                    auto& level = asks_[node->price];
                    remove_order_from_level(level, node);
                    if (level.total_size == 0) {
                        asks_.erase(node->price);
                    }
                }

                deallocate_node(node);
                order_map_.erase(record.order_id);
            }
            break;
        }
    }

    return snapshot_top_of_book();
}

std::pair<std::int64_t, std::int32_t> OrderBook::get_best_bid() const {
    if (bids_.empty()) {
        return {-1, 0};
    }
    const auto& [price, level] = *bids_.begin();
    return {price, level.total_size};
}

std::pair<std::int64_t, std::int32_t> OrderBook::get_best_ask() const {
    if (asks_.empty()) {
        return {-1, 0};
    }
    const auto& [price, level] = *asks_.begin();
    return {price, level.total_size};
}

OrderBookChange OrderBook::snapshot_top_of_book() const {
    OrderBookChange change;
    auto [best_bid, bid_size] = get_best_bid();
    auto [best_ask, ask_size] = get_best_ask();
    
    change.best_bid = best_bid;
    change.best_ask = best_ask;
    change.bid_size = bid_size;
    change.ask_size = ask_size;
    
    return change;
}

void OrderBook::print_book() const {
    std::cout << "\n========== ORDER BOOK ==========" << std::endl;
    
    std::cout << "\nASKS (Lowest First):" << std::endl;
    std::cout << std::setw(15) << "Price" << std::setw(15) << "Size" << std::endl;
    std::cout << std::string(30, '-') << std::endl;
    for (const auto& [price, level] : asks_) {
        std::cout << std::setw(15) << price << std::setw(15) << level.total_size << std::endl;
    }
    
    std::cout << "\nBIDS (Highest First):" << std::endl;
    std::cout << std::setw(15) << "Price" << std::setw(15) << "Size" << std::endl;
    std::cout << std::string(30, '-') << std::endl;
    for (const auto& [price, level] : bids_) {
        std::cout << std::setw(15) << price << std::setw(15) << level.total_size << std::endl;
    }
    
    std::cout << "\nBBO: Bid=" << get_best_bid().first << "@" << get_best_bid().second
              << " | Ask=" << get_best_ask().first << "@" << get_best_ask().second << std::endl;
    std::cout << "================================\n" << std::endl;
}

std::string OrderBook::to_json(bool pretty) const {
    std::string indent2 = pretty ? "  " : "";
    std::string indent4 = pretty ? "    " : "";
    std::string nl = pretty ? "\n" : "";
    std::string json;
    json += "{"; json += nl;
    auto [bb_price, bb_size] = get_best_bid();
    auto [ba_price, ba_size] = get_best_ask();
    json += indent2 + "\"best_bid\": {\"price\": " + std::to_string(bb_price) + ", \"size\": " + std::to_string(bb_size) + "}," + nl;
    json += indent2 + "\"best_ask\": {\"price\": " + std::to_string(ba_price) + ", \"size\": " + std::to_string(ba_size) + "}," + nl;

    // Bids array
    json += indent2 + "\"bids\": [" + nl;
    bool first_level = true;
    for (const auto& [price, level] : bids_) {
        if (!first_level) json += "," + nl; else first_level = false;
        json += indent4 + "{\"price\": " + std::to_string(price) + ", \"total_size\": " + std::to_string(level.total_size) + ", \"orders\": [";
        // Iterate orders at level
        OrderNode* cur = level.head;
        bool first_order = true;
        while (cur) {
            if (!first_order) json += ","; else first_order = false;
            json += "{\"id\": " + std::to_string(cur->order_id) + ", \"size\": " + std::to_string(cur->size) + "}";
            cur = cur->next;
        }
        json += "]}"; // close level object
    }
    json += nl + indent2 + "]," + nl;

    // Asks array
    json += indent2 + "\"asks\": [" + nl;
    first_level = true;
    for (const auto& [price, level] : asks_) {
        if (!first_level) json += "," + nl; else first_level = false;
        json += indent4 + "{\"price\": " + std::to_string(price) + ", \"total_size\": " + std::to_string(level.total_size) + ", \"orders\": [";
        OrderNode* cur = level.head;
        bool first_order = true;
        while (cur) {
            if (!first_order) json += ","; else first_order = false;
            json += "{\"id\": " + std::to_string(cur->order_id) + ", \"size\": " + std::to_string(cur->size) + "}";
            cur = cur->next;
        }
        json += "]}";
    }
    json += nl + indent2 + "]" + nl;
    json += "}" + nl;
    return json;
}

void OrderBook::save_json(const std::string& path, bool pretty) const {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        throw std::runtime_error("Failed to open file for JSON output: " + path);
    }
    ofs << to_json(pretty);
}
