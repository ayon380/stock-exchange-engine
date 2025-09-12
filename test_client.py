import grpc
import StockService_pb2
import StockService_pb2_grpc

channel = grpc.insecure_channel('localhost:50051')
stub = StockService_pb2_grpc.StockServiceStub(channel)

order = StockService_pb2.OrderRequest(
    order_id="ORD123",
    type="limit",
    quantity=100,
    price=50.5
)

resp = stub.SubmitOrder(order)
print("Response:", resp.order_id, resp.status, resp.message)
