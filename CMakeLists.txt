cmake_minimum_required(VERSION 3.16)
project(StockEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Tell CMake to use vcpkg toolchain (we pass this on CLI; helpful note)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(WARNING "Consider passing -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg.cmake")
endif()

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread interprocess)
find_package(absl CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)

# Find the gRPC plugin
find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin PATHS ${CMAKE_CURRENT_SOURCE_DIR}/build/vcpkg_installed/x64-windows/tools/grpc)

# proto
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/api/StockService.proto)
get_filename_component(PROTO_PATH ${PROTO_FILE} PATH)
get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

# Generated sources
set(PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc")
set(PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h")
set(GRPC_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc")
set(GRPC_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h")

# Custom command to generate protobuf sources
add_custom_command(
    OUTPUT "${PROTO_SRCS}" "${PROTO_HDRS}" "${GRPC_SRCS}" "${GRPC_HDRS}"
    COMMAND protobuf::protoc
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${PROTO_PATH}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Running gRPC C++ protocol buffer compiler on ${PROTO_FILE}"
)

add_executable(stock_engine
    src/main.cpp
    src/api/GRPCServer.cpp
    src/api/TCPServer.cpp
    src/api/SharedMemoryQueue.cpp
    src/api/AuthenticationManager.cpp
    src/core_engine/StockExchange.cpp
    src/core_engine/DatabaseManager.cpp
    src/core_engine/Stock.cpp
    ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
)


# Add compiler optimizations for release builds
if(MSVC)
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL /fp:fast")
    target_compile_options(stock_engine PRIVATE /O2 /GL /fp:fast)
    target_link_options(stock_engine PRIVATE /LTCG)
else()
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -march=native")
    target_compile_options(stock_engine PRIVATE -O3 -march=native -mtune=native -ffast-math)
endif()

# include dir so generated headers are found
target_include_directories(stock_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(stock_engine PRIVATE src)

target_link_libraries(stock_engine PRIVATE gRPC::grpc++ protobuf::libprotobuf libpqxx::pqxx Boost::system Boost::thread Boost::interprocess absl::flat_hash_map absl::strings redis++::redis++)

