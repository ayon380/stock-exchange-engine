# Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
#
# This source code is licensed under the terms found in the
# LICENSE file in the root directory of this source tree.
#
# USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.

cmake_minimum_required(VERSION 3.16)
project(StockEngine LANGUAGES CXX)

# --- 1. VCPKG & SPACE MANAGEMENT ---
# Force dependencies into a single folder in project root (prevents 3GB duplicate in /build)
set(VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed")

# Tell vcpkg to delete buildtrees (7GB) and packages (2.9GB) immediately after building
set(VCPKG_INSTALL_OPTIONS 
    "--clean-after-build" 
    "--clean-buildtrees-after-build" 
    "--clean-packages-after-build"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(WARNING "Consider passing -DCMAKE_TOOLCHAIN_FILE=path/to/vcpkg.cmake")
endif()

# --- 2. DEPENDENCIES ---
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread interprocess)
find_package(absl CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)

# Find gRPC plugin in the consolidated vcpkg_installed folder
if(DEFINED VCPKG_TARGET_TRIPLET)
    find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin 
        PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/grpc"
        NO_DEFAULT_PATH
    )
else()
    find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
endif()

# --- 3. PROTOBUF / GRPC GENERATION ---
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/api/StockService.proto)
get_filename_component(PROTO_PATH ${PROTO_FILE} PATH)
get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

set(PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.cc")
set(PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.pb.h")
set(GRPC_SRCS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc")
set(GRPC_HDRS "${CMAKE_CURRENT_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h")

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

# --- 4. EXECUTABLE TARGET ---
add_executable(stock_engine
    src/main.cpp
    src/api/GRPCServer.cpp
    src/api/TCPServer.cpp
    src/api/SharedMemoryQueue.cpp
    src/api/AuthenticationManager.cpp
    src/core_engine/StockExchange.cpp
    src/core_engine/DatabaseManager.cpp
    src/core_engine/Stock.cpp
    src/common/EngineConfig.cpp
    src/common/EngineTelemetry.cpp
    ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
)

# --- 5. CONDITIONAL OPTIMIZATIONS (Fast AF vs Debug) ---
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "BUILD MODE: Debug (Keeping all symbols and debug info)")
    target_compile_definitions(stock_engine PRIVATE DEBUG_MODE=1)
else()
    message(STATUS "BUILD MODE: Release (Fast AF, Stripped Binary)")
    if(MSVC)
        target_compile_options(stock_engine PRIVATE /O2 /GL /fp:fast /Gw)
        target_link_options(stock_engine PRIVATE /LTCG /OPT:REF /OPT:ICF)
    else()
        # Aggressive Mac/Linux Optimizations
        target_compile_options(stock_engine PRIVATE 
            -O3 
            -march=native 
            -ffast-math 
            -flto 
            -fomit-frame-pointer
            -DNDEBUG
        )
        target_link_options(stock_engine PRIVATE 
            -flto 
            -Wl,-s  # Strips symbols to reduce binary size significantly
        )
    endif()
endif()

# Includes
target_include_directories(stock_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR} src)

# Linking
target_link_libraries(stock_engine PRIVATE 
    gRPC::grpc++ 
    protobuf::libprotobuf 
    libpqxx::pqxx 
    Boost::system 
    Boost::thread 
    Boost::interprocess 
    absl::flat_hash_map 
    absl::strings 
    redis++::redis++_static
)

# --- 6. TESTS ---
add_executable(test_engine
    tests/test_engine.cpp
    src/core_engine/StockExchange.cpp
    src/core_engine/DatabaseManager.cpp
    src/core_engine/Stock.cpp
    src/common/EngineConfig.cpp
    src/common/EngineTelemetry.cpp
    src/api/AuthenticationManager.cpp
)
target_include_directories(test_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR} src)
target_link_libraries(test_engine PRIVATE libpqxx::pqxx Boost::system Boost::thread Boost::interprocess redis++::redis++_static absl::flat_hash_map)

add_executable(test_queue_fix tests/test_queue_fix.cpp)
target_include_directories(test_queue_fix PRIVATE src)
target_link_libraries(test_queue_fix PRIVATE Boost::system Boost::thread Boost::interprocess)