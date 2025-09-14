#pragma once
#include "StockService.grpc.pb.h"
#include "../core_engine/StockExchange.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <map>
#include <mutex>
#include <atomic>

class GRPCServer final : public stock::StockService::Service {
private:
    std::unique_ptr<StockExchange> exchange_;
    
    // For managing streaming connections
    mutable std::mutex streams_mutex_;
    std::atomic<int> stream_id_counter_;
    std::map<int, grpc::ServerWriter<stock::MarketDataUpdate>*> market_data_streams_;
    std::map<int, grpc::ServerWriter<stock::IndexUpdate>*> index_streams_;
    
    void convertToCoreOrder(const stock::OrderRequest& request, Order& order);
    void convertFromCoreOrderStatus(const Order& core_order, stock::OrderStatusResponse& response);
    void convertFromCoreMarketData(const MarketDataUpdate& core_update, stock::MarketDataUpdate& proto_update);
    void convertFromCoreIndex(const std::vector<IndexEntry>& core_entries, stock::IndexUpdate& proto_update);

public:
    GRPCServer(const std::string& db_connection_string = "");
    ~GRPCServer();
    
    bool initialize();
    void start();
    void stop();
    
    // Get access to the underlying exchange for TCP server
    StockExchange* getExchange() { return exchange_.get(); }

    // Unary RPCs
    grpc::Status SubmitOrder(grpc::ServerContext* context,
                             const stock::OrderRequest* request,
                             stock::OrderResponse* response) override;
                             
    grpc::Status OrderStatus(grpc::ServerContext* context,
                            const stock::OrderStatusRequest* request,
                            stock::OrderStatusResponse* response) override;
    
    // Streaming RPCs
    grpc::Status StreamMarketData(grpc::ServerContext* context,
                                 const stock::MarketDataRequest* request,
                                 grpc::ServerWriter<stock::MarketDataUpdate>* writer) override;
                                 
    grpc::Status StreamTopIndex(grpc::ServerContext* context,
                               const stock::IndexRequest* request,
                               grpc::ServerWriter<stock::IndexUpdate>* writer) override;
                               
    grpc::Status StreamAllStocks(grpc::ServerContext* context,
                                const stock::AllStocksRequest* request,
                                grpc::ServerWriter<stock::AllStocksUpdate>* writer) override;
                                
    grpc::Status StreamMarketIndex(grpc::ServerContext* context,
                                  const stock::MarketIndexRequest* request,
                                  grpc::ServerWriter<stock::MarketIndexUpdate>* writer) override;
};
