cmake_minimum_required(VERSION 3.10)
project(RealTimeMarketAggregator CXX)

set(CMAKE_CXX_STANDARD 17)

# 1. Find Dependencies
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)

# Find Kafka
find_library(KAFKA_LIBRARIES NAMES rdkafka REQUIRED)
find_path(KAFKA_INCLUDE_DIRS NAMES librdkafka/rdkafka.h REQUIRED)

# Find Redis (hiredis)
find_library(HIREDIS_LIBRARIES NAMES hiredis REQUIRED)
# We look for the folder ABOVE hiredis/ to allow #include <hiredis/hiredis.h>
find_path(HIREDIS_BASE_DIR NAMES hiredis/hiredis.h REQUIRED)

# 2. Protobuf Code Generation
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS market_data.proto)

# 3. Target: Producer
add_executable(producer cmd/producer/main.cpp ${PROTO_SRCS})
target_include_directories(producer PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROTOBUF_INCLUDE_DIRS}
    ${KAFKA_INCLUDE_DIRS}
)
target_link_libraries(producer
    ${PROTOBUF_LIBRARIES}
    ${KAFKA_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

# 4. Target: Aggregator (Phase 2)
add_executable(aggregator cmd/aggregator/main.cpp ${PROTO_SRCS})
target_include_directories(aggregator PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROTOBUF_INCLUDE_DIRS}
    ${KAFKA_INCLUDE_DIRS}
    ${HIREDIS_BASE_DIR}
    ${PostgreSQL_INCLUDE_DIRS}
)
target_link_libraries(aggregator
    ${PROTOBUF_LIBRARIES}
    ${KAFKA_LIBRARIES}
    ${HIREDIS_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PostgreSQL_LIBRARIES}
)

# 5. Target: Snapshot API
add_executable(snapshot_api cmd/snapshot/main.cpp)
target_link_libraries(snapshot_api ${HIREDIS_LIBRARIES})
target_include_directories(snapshot_api PUBLIC ${HIREDIS_BASE_DIR})
