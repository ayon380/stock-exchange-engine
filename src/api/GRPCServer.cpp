#include "GRPCServer.h"
#include "common/EngineTelemetry.h"
#include "common/EngineLogging.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
#include <vector>
#include <algorithm>
#include <atomic>
#include <iomanip>

GRPCServer::GRPCServer(const std::string& db_connection_string)
    : exchange_(std::make_unique<StockExchange>(db_connection_string)) {}

GRPCServer::~GRPCServer() { stop(); }

bool GRPCServer::initialize() { return exchange_ ? exchange_->initialize() : false; }
void GRPCServer::start() { if (exchange_) exchange_->start(); }
void GRPCServer::stop() { if (exchange_) exchange_->stop(); }

// Get performance log file
static std::ofstream& get_perf_file() {
    static std::ofstream perf_file("performance.txt", std::ios::app);
    return perf_file;
}

void GRPCServer::convertToCoreOrder(const stock::OrderRequest& req, Order& order) {
    order = Order{
        req.order_id(),
        req.user_id(),
        req.symbol(),
        static_cast<int>(req.side()),
        static_cast<int>(req.type()),
        static_cast<int64_t>(req.quantity()),
        static_cast<Price>(req.price() * 100.0 + 0.5), // Convert double to Price
        static_cast<int64_t>(req.timestamp_ms())
    };
}

void GRPCServer::convertFromCoreOrderStatus(const Order& core, stock::OrderStatusResponse& resp) {
    resp.set_order_id(core.order_id);
    resp.set_exists(!core.order_id.empty());
    resp.set_remaining_qty(core.remaining_qty);
    resp.set_status(core.status);
    // Trades list omitted here; add if you track per-order trades separately
}

void GRPCServer::convertFromCoreMarketData(const MarketDataUpdate& core, stock::MarketDataUpdate& out) {
    out.set_symbol(core.symbol);
    out.set_last_price(static_cast<double>(core.last_price) / 100.0);
    out.set_last_qty(core.last_qty);
    out.set_timestamp_ms(core.timestamp_ms);
    for (const auto& lvl : core.top_bids) {
        auto* p = out.add_top_bids();
        p->set_price(static_cast<double>(lvl.price) / 100.0);
        p->set_qty(lvl.quantity);
    }
    for (const auto& lvl : core.top_asks) {
        auto* p = out.add_top_asks();
        p->set_price(static_cast<double>(lvl.price) / 100.0);
        p->set_qty(lvl.quantity);
    }
}

void GRPCServer::convertFromCoreIndex(const std::vector<IndexEntry>& core, stock::IndexUpdate& out) {
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out.set_timestamp_ms(ts);
    for (const auto& e : core) {
        auto* pe = out.add_entries();
        pe->set_symbol(e.symbol);
        pe->set_last_price(e.priceToDouble());
        pe->set_change_pct(e.change_pct);
        pe->set_volume(e.volume);
    }
}

grpc::Status GRPCServer::SubmitOrder(grpc::ServerContext* context,
                                     const stock::OrderRequest* request,
                                     stock::OrderResponse* response) {
    (void)context;
    auto grpc_start = std::chrono::high_resolution_clock::now();

    ENGINE_LOG_DEV(std::cout << "Received order: " << request->order_id()
                             << " type: " << request->type()
                             << " qty: " << request->quantity()
                             << " price: " << request->price() << std::endl;);

    response->set_order_id(request->order_id());
    response->set_accepted(true);
    response->set_message("Order received successfully");

    auto conversion_start = std::chrono::high_resolution_clock::now();
    Order core_order;
    convertToCoreOrder(*request, core_order);
    auto conversion_end = std::chrono::high_resolution_clock::now();

    auto submit_start = std::chrono::high_resolution_clock::now();
    if (exchange_) {
        auto result = exchange_->submitOrder(request->symbol(), core_order);
        if (result != "accepted") {
            response->set_accepted(false);
            response->set_message(result);
        }
    }
    auto submit_end = std::chrono::high_resolution_clock::now();

    // Measure and log detailed latencies
    auto grpc_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(grpc_end - grpc_start);
    auto conversion_duration = std::chrono::duration_cast<std::chrono::microseconds>(conversion_end - conversion_start);
    auto submit_duration = std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start);

    long long total_us = total_duration.count();
    long long conversion_us = conversion_duration.count();
    long long submit_us = submit_duration.count();

    // Thread-safe metrics collection
    static std::mutex metrics_mutex;
    static std::vector<long long> total_latencies;
    static std::vector<long long> conversion_latencies;
    static std::vector<long long> submit_latencies;
    static std::atomic<uint64_t> order_counter{0};
    static long long min_latency = LLONG_MAX;
    static long long max_latency = 0;
    static long long total_sum = 0;

    uint64_t current_count = order_counter.fetch_add(1, std::memory_order_relaxed) + 1;

    {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        total_latencies.push_back(total_us);
        conversion_latencies.push_back(conversion_us);
        submit_latencies.push_back(submit_us);

        min_latency = std::min(min_latency, total_us);
        max_latency = std::max(max_latency, total_us);
        total_sum += total_us;
    }

    EngineTelemetry::instance().recordOrder(total_us);

    // Log detailed metrics every 1000 orders
    if (current_count % 1000 == 0) {
        std::lock_guard<std::mutex> lock(metrics_mutex);

        // Calculate percentiles for total latency
        std::vector<long long> sorted_total = total_latencies;
        std::sort(sorted_total.begin(), sorted_total.end());
        size_t n = sorted_total.size();
        long long p50_total = sorted_total[n * 50 / 100];
        long long p90_total = sorted_total[n * 90 / 100];
        long long p99_total = sorted_total[n * 99 / 100];
        double avg_total = static_cast<double>(total_sum) / current_count;

        // Calculate percentiles for conversion latency
        std::vector<long long> sorted_conversion = conversion_latencies;
        std::sort(sorted_conversion.begin(), sorted_conversion.end());
        long long p50_conv = sorted_conversion[n * 50 / 100];
        long long p90_conv = sorted_conversion[n * 90 / 100];
        long long p99_conv = sorted_conversion[n * 99 / 100];

        // Calculate percentiles for submit latency
        std::vector<long long> sorted_submit = submit_latencies;
        std::sort(sorted_submit.begin(), sorted_submit.end());
        long long p50_submit = sorted_submit[n * 50 / 100];
        long long p90_submit = sorted_submit[n * 90 / 100];
        long long p99_submit = sorted_submit[n * 99 / 100];

        get_perf_file() << "=== PERFORMANCE METRICS (Orders: " << current_count << ") ===" << std::endl;
        get_perf_file() << "Total Latency - Min: " << min_latency << "us, Max: " << max_latency
                       << "us, Avg: " << (total_sum / current_count)
                       << "us, P50: " << p50_total << "us, P90: " << p90_total << "us, P99: " << p99_total << "us" << std::endl;
        get_perf_file() << "Conversion Latency - P50: " << p50_conv << "us, P90: " << p90_conv << "us, P99: " << p99_conv << "us" << std::endl;
        get_perf_file() << "Submit Latency - P50: " << p50_submit << "us, P90: " << p90_submit << "us, P99: " << p99_submit << "us" << std::endl;
        get_perf_file() << "==================================================" << std::endl;

        // Reset for next 1000 orders
        total_latencies.clear();
        conversion_latencies.clear();
        submit_latencies.clear();
        min_latency = LLONG_MAX;
        max_latency = 0;
        total_sum = 0;
    }

    return grpc::Status::OK;
}

grpc::Status GRPCServer::OrderStatus(grpc::ServerContext* context,
                                     const stock::OrderStatusRequest* request,
                                     stock::OrderStatusResponse* response) {
    (void)context;
    if (!exchange_) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Exchange not initialized");

    // We don't know the symbol from request; in real system you'd index by order_id
    // For now, check all symbols
    for (const auto& sym : exchange_->getSymbols()) {
        Order core = exchange_->getOrderStatus(sym, request->order_id());
        if (!core.order_id.empty()) {
            convertFromCoreOrderStatus(core, *response);
            return grpc::Status::OK;
        }
    }

    response->set_order_id(request->order_id());
    response->set_exists(false);
    response->set_remaining_qty(0);
    response->set_status("not_found");
    return grpc::Status::OK;
}

grpc::Status GRPCServer::StreamMarketData(grpc::ServerContext* context,
                                          const stock::MarketDataRequest* request,
                                          grpc::ServerWriter<stock::MarketDataUpdate>* writer) {
    if (!exchange_) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Exchange not initialized");

    auto symbols = request->symbols();
    std::vector<std::string> subs;
    if (symbols.empty()) {
        subs = exchange_->getSymbols();
    } else {
        subs.assign(symbols.begin(), symbols.end());
    }

    // Send a snapshot first
    for (const auto& sym : subs) {
        auto md = exchange_->getMarketData(sym);
        stock::MarketDataUpdate out;
        convertFromCoreMarketData(md, out);
        if (!writer->Write(out)) return grpc::Status::OK; // client closed
    }

    // Simple periodic updates (polling) to keep compile-time simple
    while (!context->IsCancelled()) {
        for (const auto& sym : subs) {
            auto md = exchange_->getMarketData(sym);
            stock::MarketDataUpdate out;
            convertFromCoreMarketData(md, out);
            if (!writer->Write(out)) return grpc::Status::OK;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return grpc::Status::OK;
}

grpc::Status GRPCServer::StreamTopIndex(grpc::ServerContext* context,
                                        const stock::IndexRequest* request,
                                        grpc::ServerWriter<stock::IndexUpdate>* writer) {
    if (!exchange_) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Exchange not initialized");

    std::string criterion = request->criterion().empty() ? std::string("volume") : request->criterion();
    int top_n = request->top_n() > 0 ? request->top_n() : 5;

    while (!context->IsCancelled()) {
        auto entries = exchange_->getTopIndex(criterion, top_n);
        stock::IndexUpdate out;
        convertFromCoreIndex(entries, out);
        if (!writer->Write(out)) return grpc::Status::OK;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return grpc::Status::OK;
}

grpc::Status GRPCServer::StreamAllStocks(grpc::ServerContext* context,
                                         const stock::AllStocksRequest* request,
                                         grpc::ServerWriter<stock::AllStocksUpdate>* writer) {
    if (!exchange_) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Exchange not initialized");

    bool include_order_book = request->include_order_book();

    while (!context->IsCancelled()) {
        auto snapshots = exchange_->getAllStocksSnapshot(include_order_book);
        
        stock::AllStocksUpdate update;
        auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        update.set_timestamp_ms(ts);
        
        for (const auto& snapshot : snapshots) {
            auto* stock_data = update.add_stocks();
            stock_data->set_symbol(snapshot.symbol);
            stock_data->set_last_price(static_cast<double>(snapshot.last_price) / 100.0);
            stock_data->set_last_qty(0); // Default value
            stock_data->set_change_pct(snapshot.change_percent);
            stock_data->set_volume(snapshot.volume);
            
            if (include_order_book) {
                for (const auto& bid : snapshot.top_bids) {
                    auto* level = stock_data->add_top_bids();
                    level->set_price(static_cast<double>(bid.price) / 100.0);
                    level->set_qty(bid.quantity);
                }
                for (const auto& ask : snapshot.top_asks) {
                    auto* level = stock_data->add_top_asks();
                    level->set_price(static_cast<double>(ask.price) / 100.0);
                    level->set_qty(ask.quantity);
                }
            }
        }
        
        if (!writer->Write(update)) return grpc::Status::OK;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return grpc::Status::OK;
}

grpc::Status GRPCServer::StreamMarketIndex(grpc::ServerContext* context,
                                           const stock::MarketIndexRequest* request,
                                           grpc::ServerWriter<stock::MarketIndexUpdate>* writer) {
    if (!exchange_) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Exchange not initialized");

    std::string index_name = request->index_name().empty() ? "TECH500" : request->index_name();

    while (!context->IsCancelled()) {
        auto market_index = exchange_->getMarketIndex(index_name);
        
        stock::MarketIndexUpdate update;
        update.set_index_name(market_index.index_name);
        update.set_index_value(market_index.index_value);
        update.set_change_points(market_index.change_points);
        update.set_change_percent(market_index.change_percent);
        update.set_timestamp_ms(market_index.timestamp_ms);
        
        // Note: MarketIndexUpdate doesn't have constituents field in current proto definition
        
        if (!writer->Write(update)) return grpc::Status::OK;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return grpc::Status::OK;
}
