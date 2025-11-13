#include "../include/engine.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <sstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#ifdef HFT_HAS_DATABENTO
#include <databento/exceptions.hpp>
#endif

// Engine DBN integration: we use databento::DbnFileStore to read raw records.
// Initial phase: provide raw MBO JSON dump to verify file decoding before
// constructing an order book.

Engine::Engine(std::string dbn_path) : dbn_path_(std::move(dbn_path)) {}

void Engine::init() {
    // Currently nothing special to init besides constructing OrderBook.
}

#ifdef HFT_HAS_DATABENTO
DBNRecord Engine::map_mbo(const databento::MboMsg& mbo) const {
    DBNRecord r;
    r.order_id = mbo.order_id;
    r.price = mbo.price; // price is already integer (likely in nanounits)
    r.size = static_cast<std::int32_t>(mbo.size);
    r.side = (mbo.side == databento::Side::Bid) ? 'B' : 'A';
    switch (mbo.action) {
        case databento::Action::Add: r.action = 'A'; break;
        case databento::Action::Modify: r.action = 'M'; break;
        case databento::Action::Cancel: r.action = 'C'; break;
        case databento::Action::Trade: // Trade implies a fill for an existing order
        case databento::Action::Fill: r.action = 'F'; break;
        default: r.action = 'U'; break;
    }
    return r;
}
#endif

void Engine::replay(const AsyncLogger& logger, std::size_t max_snapshots) {
#ifndef HFT_HAS_DATABENTO
    logger.log("Databento headers not available. Replay disabled.");
    return;
#else
    if (dbn_path_.empty()) {
        logger.log("No DBN file path set.");
        return;
    }
    logger.log(std::string("Replaying file for order book construction: ") + dbn_path_);
    try {
        // Use upgrade policy so version 3 DBN can be decoded by v2 decoder.
        databento::DbnFileStore store(nullptr, dbn_path_, databento::VersionUpgradePolicy::UpgradeToV2);
        std::size_t snapshot_count = 0;
        store.Replay([&](const databento::Record& rec){
            if (!running_.load(std::memory_order_relaxed)) {
                return databento::Stop;
            }
            if (rec.Holds<databento::MboMsg>()) {
                const auto& mbo = rec.Get<databento::MboMsg>();
                DBNRecord r = map_mbo(mbo);
                book_.apply_update(r);
                if (++snapshot_count >= max_snapshots) {
                    return databento::Stop;
                }
            }
            return databento::Continue;
        });
        logger.log("Replay finished; applied " + std::to_string(snapshot_count) + " MBO messages to book.");
    } catch (const databento::DbnResponseError& e) {
        metrics_.replay_errors.fetch_add(1, std::memory_order_relaxed);
        metrics_.set_last_error(e.what());
        logger.log(std::string("DBN replay failed: ") + e.what());
    } catch (const std::exception& e) {
        metrics_.replay_errors.fetch_add(1, std::memory_order_relaxed);
        metrics_.set_last_error(e.what());
        logger.log(std::string("Unexpected replay error: ") + e.what());
    }
#endif
}

std::string Engine::reconstruct_orderbook_json(std::size_t levels) const {
#ifndef HFT_HAS_DATABENTO
    return "{\"error\": \"Databento headers not available\"}";
#else
    if (dbn_path_.empty()) return "{\"error\": \"No DBN path provided\"}";
    using namespace databento;
    struct OrderRef { int64_t price; Side side; };
    struct LevelOrders { std::vector<MboMsg> orders; };
    struct BookSide { std::map<int64_t, LevelOrders> levels; };
    struct PublisherBook { uint16_t publisher_id; BookSide bids; BookSide asks; std::unordered_map<uint64_t, OrderRef> by_id; };
    struct Instrument { 
        uint32_t instrument_id; 
        std::vector<PublisherBook> pub_books; 
        Instrument() { pub_books.reserve(4); } // pre-reserve typical publisher count
    };
    std::unordered_map<uint32_t, Instrument> instruments; 
    UnixNanos last_ts_recv{}; size_t mbo_count=0;
    
    auto replay_start = std::chrono::high_resolution_clock::now();
    
    try {
        DbnFileStore store(nullptr, dbn_path_, VersionUpgradePolicy::UpgradeToV2);
        store.Replay([&](const Record& rec){
            if (!rec.Holds<MboMsg>()) return databento::Continue;
            const auto& mbo = rec.Get<MboMsg>();
            if (!running_.load(std::memory_order_relaxed)) {
                return databento::Stop;
            }
            
            // Measure per-message processing latency
            auto start = std::chrono::high_resolution_clock::now();
            
            last_ts_recv = mbo.ts_recv; ++mbo_count;
            auto& inst = instruments[mbo.hd.instrument_id];
            inst.instrument_id = mbo.hd.instrument_id;
                // Find publisher book
                auto pub_it = std::find_if(inst.pub_books.begin(), inst.pub_books.end(), [&](const PublisherBook& pb){return pb.publisher_id==mbo.hd.publisher_id;});
                if (pub_it==inst.pub_books.end()) { inst.pub_books.push_back(PublisherBook{mbo.hd.publisher_id,{},{},{}}); pub_it = std::prev(inst.pub_books.end()); }
                PublisherBook& pb = *pub_it;
                // Handle actions
                switch (mbo.action) {
                    case Action::Clear: {
                        if (mbo.side==Side::Bid) { pb.bids.levels.clear(); } else if (mbo.side==Side::Ask){ pb.asks.levels.clear(); }
                        if (mbo.price!=kUndefPrice) {
                            auto& side = (mbo.side==Side::Bid)? pb.bids : pb.asks;
                            side.levels[mbo.price].orders.push_back(mbo);
                        }
                        break; }
                    case Action::Add: {
                        auto& side = (mbo.side==Side::Bid)? pb.bids : pb.asks;
                        side.levels[mbo.price].orders.push_back(mbo);
                        pb.by_id.emplace(mbo.order_id, OrderRef{mbo.price,mbo.side});
                        break; }
                    case Action::Cancel: {
                        auto oid_it = pb.by_id.find(mbo.order_id); if (oid_it==pb.by_id.end()) break; // ignore unknown
                        auto& side = (oid_it->second.side==Side::Bid)? pb.bids : pb.asks;
                        auto lvl_it = side.levels.find(oid_it->second.price); if (lvl_it==side.levels.end()) break;
                        // find order
                        auto& vec = lvl_it->second.orders;
                        auto ord_it = std::find_if(vec.begin(), vec.end(), [&](const MboMsg& o){return o.order_id==mbo.order_id;});
                        if (ord_it!=vec.end()) {
                            if (ord_it->size >= mbo.size) ord_it->size -= mbo.size; else ord_it->size=0;
                            if (ord_it->size==0) { vec.erase(ord_it); pb.by_id.erase(oid_it); }
                            if (vec.empty()) side.levels.erase(lvl_it);
                        }
                        break; }
                    case Action::Modify: {
                        auto oid_it = pb.by_id.find(mbo.order_id); if (oid_it==pb.by_id.end()) { // treat as add
                            auto& side = (mbo.side==Side::Bid)? pb.bids : pb.asks;
                            side.levels[mbo.price].orders.push_back(mbo);
                            pb.by_id.emplace(mbo.order_id, OrderRef{mbo.price,mbo.side});
                            break; }
                        // existing order
                        auto& side_old = (oid_it->second.side==Side::Bid)? pb.bids : pb.asks;
                        auto lvl_old_it = side_old.levels.find(oid_it->second.price); if (lvl_old_it!=side_old.levels.end()) {
                            auto& vec = lvl_old_it->second.orders;
                            auto ord_it = std::find_if(vec.begin(), vec.end(), [&](const MboMsg& o){return o.order_id==mbo.order_id;});
                            if (ord_it!=vec.end()) {
                                if (oid_it->second.price != mbo.price) { // price change => remove then reinsert losing priority
                                    // Reuse existing MboMsg; update in place then move
                                    ord_it->price = mbo.price; ord_it->size = mbo.size;
                                    auto& side_new = (mbo.side==Side::Bid)? pb.bids : pb.asks;
                                    side_new.levels[mbo.price].orders.push_back(std::move(*ord_it));
                                    vec.erase(ord_it);
                                    if (vec.empty()) side_old.levels.erase(lvl_old_it);
                                    oid_it->second.price = mbo.price; oid_it->second.side = mbo.side;
                                } else {
                                    // same price adjust size; if size increases lose priority => move to end
                                    if (ord_it->size < mbo.size) { 
                                        ord_it->size = mbo.size; 
                                        auto temp = std::move(*ord_it); 
                                        vec.erase(ord_it); 
                                        vec.push_back(std::move(temp)); 
                                    }
                                    else { ord_it->size = mbo.size; }
                                }
                            }
                        }
                        break; }
                    case Action::Trade: case Action::Fill: case Action::None: default: break; // ignore
                }
            
            // Record latency after processing
            auto end = std::chrono::high_resolution_clock::now();
            auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            metrics_.record_latency(latency_ns);
            metrics_.total_messages.fetch_add(1, std::memory_order_relaxed);
            
            return databento::Continue;
        });
        
    auto replay_end = std::chrono::high_resolution_clock::now();
    metrics_.replay_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(replay_end - replay_start).count();
        
    } catch (const databento::DbnResponseError& e) {
        metrics_.replay_errors.fetch_add(1, std::memory_order_relaxed);
        metrics_.set_last_error(e.what());
        return std::string("{\"error\": \"DbnResponseError:")+e.what()+"\"}";
    } catch (const std::exception& e) {
        metrics_.replay_errors.fetch_add(1, std::memory_order_relaxed);
        metrics_.set_last_error(e.what());
        return std::string("{\"error\": \"Exception:")+e.what()+"\"}";
    }
    // Build JSON (pretty, top-to-bottom). Prices formatted as decimal with 2 places (raw / 1e9).
    auto ns_to_iso = [](UnixNanos ts){ return databento::ToIso8601(ts); };
    auto fmt_price = [](int64_t px){ if (px==databento::kUndefPrice) return std::string("null"); std::ostringstream os; os.setf(std::ios::fixed); os<<std::setprecision(2)<< (double)px/1e9; return os.str(); };
    std::ostringstream oss; oss << "{\n  \"instruments\": [\n";
    bool first_inst=true;
    for (auto& kv : instruments) {
        auto& inst = kv.second; if (!first_inst) oss << ",\n"; first_inst=false;
        oss << "    {\n      \"instrument_id\": "<<inst.instrument_id<<",\n      \"publishers\": [\n";
        // aggregated bbo computation accumulates while iterating
        int64_t agg_bid_px = databento::kUndefPrice; uint32_t agg_bid_sz=0, agg_bid_ct=0;
        int64_t agg_ask_px = databento::kUndefPrice; uint32_t agg_ask_sz=0, agg_ask_ct=0;
        bool first_pub=true;
        for (auto& pb : inst.pub_books) {
            // publisher best
            auto best_bid_it = pb.bids.levels.rbegin();
            int64_t bid_px = (best_bid_it==pb.bids.levels.rend()? databento::kUndefPrice : best_bid_it->first);
            uint32_t bid_sz=0,bid_ct=0; if (bid_px!=databento::kUndefPrice){ for (auto& o: best_bid_it->second.orders){ bid_sz += o.size; if(!o.flags.IsTob()) ++bid_ct; }}
            auto best_ask_it = pb.asks.levels.begin();
            int64_t ask_px = (best_ask_it==pb.asks.levels.end()? databento::kUndefPrice : best_ask_it->first);
            uint32_t ask_sz=0,ask_ct=0; if (ask_px!=databento::kUndefPrice){ for (auto& o: best_ask_it->second.orders){ ask_sz += o.size; if(!o.flags.IsTob()) ++ask_ct; }}
            if (bid_px!=databento::kUndefPrice){ if (agg_bid_px==databento::kUndefPrice || bid_px>agg_bid_px){ agg_bid_px=bid_px; agg_bid_sz=bid_sz; agg_bid_ct=bid_ct; } else if (bid_px==agg_bid_px){ agg_bid_sz+=bid_sz; agg_bid_ct+=bid_ct; }}
            if (ask_px!=databento::kUndefPrice){ if (agg_ask_px==databento::kUndefPrice || ask_px<agg_ask_px){ agg_ask_px=ask_px; agg_ask_sz=ask_sz; agg_ask_ct=ask_ct; } else if (ask_px==agg_ask_px){ agg_ask_sz+=ask_sz; agg_ask_ct+=ask_ct; }}
            if (!first_pub) oss << ",\n"; first_pub=false;
            oss << "        {\n          \"publisher_id\": "<<pb.publisher_id<<",\n          \"bbo\": {\n            \"bid\": {\"price\": "<<fmt_price(bid_px)<<", \"size\": "<<bid_sz<<", \"count\": "<<bid_ct<<"},\n            \"ask\": {\"price\": "<<fmt_price(ask_px)<<", \"size\": "<<ask_sz<<", \"count\": "<<ask_ct<<"}\n          },\n          \"levels\": {\n            \"bids\": [\n";
            {
              size_t emitted=0; 
              for (auto rit=pb.bids.levels.rbegin(); rit!=pb.bids.levels.rend(); ++rit){
                if (levels!=0 && emitted>=levels) break;
                uint32_t sz=0,ct=0; for (auto& o: rit->second.orders){ sz+=o.size; if(!o.flags.IsTob()) ++ct; }
                if (emitted>0) oss << ",";
                oss << "              {\"price\": "<<fmt_price(rit->first)<<", \"size\": "<<sz<<", \"count\": "<<ct<<"}\n";
                ++emitted;
              }
            }
            oss << "            ],\n            \"asks\": [\n";
            {
              size_t emitted=0; 
              for (auto it=pb.asks.levels.begin(); it!=pb.asks.levels.end(); ++it){
                if (levels!=0 && emitted>=levels) break;
                uint32_t sz=0,ct=0; for (auto& o: it->second.orders){ sz+=o.size; if(!o.flags.IsTob()) ++ct; }
                if (emitted>0) oss << ",";
                oss << "              {\"price\": "<<fmt_price(it->first)<<", \"size\": "<<sz<<", \"count\": "<<ct<<"}\n";
                ++emitted;
              }
            }
            oss << "            ]\n          }\n        }"; // end publisher
        }
        oss << "\n      ],\n      \"aggregated_bbo\": {\n        \"bid\": {\"price\": "<<fmt_price(agg_bid_px)<<", \"size\": "<<agg_bid_sz<<", \"count\": "<<agg_bid_ct<<"},\n        \"ask\": {\"price\": "<<fmt_price(agg_ask_px)<<", \"size\": "<<agg_ask_sz<<", \"count\": "<<agg_ask_ct<<"}\n      }\n    }";
    }
    oss << "\n  ],\n  \"last_ts_recv_iso\": \""<< ns_to_iso(last_ts_recv) <<"\",\n  \"mbo_count\": "<<mbo_count<<"\n}\n";
    return oss.str();
#endif
}

void Engine::save_aggregated_orderbook_json(const std::string& path, std::size_t levels) const {
    std::ofstream ofs(path); if (!ofs.is_open()) return; ofs << reconstruct_orderbook_json(levels);
}
