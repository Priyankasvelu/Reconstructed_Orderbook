cmake_minimum_required(VERSION 3.14)
project(hft-engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include FetchContent for downloading dependencies
include(FetchContent)

# Fetch Databento C++ Client Library
FetchContent_Declare(
    databento
    GIT_REPOSITORY https://github.com/databento/databento-cpp.git
    # Use HEAD to ensure support for latest DBN version (v3)
    GIT_TAG HEAD
)

# Fetch cpp-httplib for REST/WebSocket API
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.3
)

FetchContent_MakeAvailable(databento httplib)

# Add source files
add_executable(hft-engine 
    src/main.cpp
    src/orderbook.cpp
    src/engine.cpp
    src/metrics.cpp
    src/apiserver.cpp
)

# Include directories
target_include_directories(hft-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Link Databento library and httplib
target_link_libraries(hft-engine PRIVATE databento::databento httplib::httplib)
