#include "GRPCServer.h"
#include <iostream>
#include <chrono>
#include <thread>

GRPCServer::GRPCServer(const std::string& db_connection_string)
    : exchange_(std::make_unique<StockExchange>(db_connection_string)) {}

GRPCServer::~GRPCServer() { stop(); }

bool GRPCServer::initialize() { return exchange_ ? exchange_->initialize() : false; }
void GRPCServer::start() { if (exchange_) exchange_->start(); }
void GRPCServer::stop() { if (exchange_) exchange_->stop(); }

void GRPCServer::convertToCoreOrder(const stock::OrderRequest& req, Order& order) {
    order = Order{
        req.order_id(),
        req.user_id(),
        req.symbol(),
        static_cast<int>(req.side()),
        static_cast<int>(req.type()),
        static_cast<int64_t>(req.quantity()),
        req.price(),
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
    out.set_last_price(core.last_price);
    out.set_last_qty(core.last_qty);
    out.set_timestamp_ms(core.timestamp_ms);
    for (const auto& lvl : core.top_bids) {
        auto* p = out.add_top_bids();
        p->set_price(lvl.price);
        p->set_qty(lvl.quantity);
    }
    for (const auto& lvl : core.top_asks) {
        auto* p = out.add_top_asks();
        p->set_price(lvl.price);
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
        pe->set_last_price(e.last_price);
        pe->set_change_pct(e.change_pct);
        pe->set_volume(e.volume);
    }
}

grpc::Status GRPCServer::SubmitOrder(grpc::ServerContext* context,
                                     const stock::OrderRequest* request,
                                     stock::OrderResponse* response) {
    (void)context;
    std::cout << "Received order: " << request->order_id()
              << " type: " << request->type()
              << " qty: " << request->quantity()
              << " price: " << request->price() << std::endl;

    response->set_order_id(request->order_id());
    response->set_accepted(true);
    response->set_message("Order received successfully");

    if (exchange_) {
        Order core_order;
        convertToCoreOrder(*request, core_order);
        auto result = exchange_->submitOrder(request->symbol(), core_order);
        if (result != "accepted") {
            response->set_accepted(false);
            response->set_message(result);
        }
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
            stock_data->set_last_price(snapshot.last_price);
            stock_data->set_last_qty(0); // Default value
            stock_data->set_change_pct(snapshot.change_percent);
            stock_data->set_volume(snapshot.volume);
            
            if (include_order_book) {
                for (const auto& bid : snapshot.top_bids) {
                    auto* level = stock_data->add_top_bids();
                    level->set_price(bid.price);
                    level->set_qty(bid.quantity);
                }
                for (const auto& ask : snapshot.top_asks) {
                    auto* level = stock_data->add_top_asks();
                    level->set_price(ask.price);
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
