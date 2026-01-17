# Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
#
# This source code is licensed under the terms found in the
# LICENSE file in the root directory of this source tree.
#
# USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.

import grpc, time, threading
import matplotlib.pyplot as plt
import StockService_pb2 as pb
import StockService_pb2_grpc as pb_grpc

SERVER = "localhost:50051"
SYMBOLS = ["AAPL","GOOGL","MSFT","TSLA","AMZN","META","NVDA","NFLX"]

channel = grpc.insecure_channel(SERVER)
stub = pb_grpc.StockServiceStub(channel)

# --- live prices ---
latest_prices = {s: 0.0 for s in SYMBOLS}
def price_stream():
    req = pb.AllStocksRequest(include_order_book=False)
    for update in stub.StreamAllStocks(req):
        for snap in update.stocks:
            if snap.symbol in latest_prices:
                latest_prices[snap.symbol] = snap.last_price

# --- index values ---
index_values = []
timestamps = []
def index_stream():
    req = pb.MarketIndexRequest(index_name="MYINDEX")
    for update in stub.StreamMarketIndex(req):
        index_values.append(update.index_value)
        timestamps.append(time.time())

# --- plotting loop ---
def plot_loop():
    plt.ion()
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8,6))
    while True:
        ax1.clear()
        ax1.plot(timestamps, index_values, color="blue")
        ax1.set_title("Market Index")
        ax1.set_ylabel("Index Value")

        ax2.clear()
        ax2.bar(latest_prices.keys(), latest_prices.values(), color="green")
        ax2.set_title("Latest Stock Prices")
        ax2.set_ylabel("Price ($)")
        plt.pause(1)

if __name__ == "__main__":
    threading.Thread(target=price_stream, daemon=True).start()
    threading.Thread(target=index_stream, daemon=True).start()
    plot_loop()
