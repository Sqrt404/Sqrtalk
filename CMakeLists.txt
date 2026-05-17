cmake_minimum_required(VERSION 3.14)
project(SqrtalkServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Dependencies
# ============================================================

# ---- websocketpp (header-only) ----
find_path(WEBSOCKETPP_INCLUDE_DIR
    NAMES websocketpp/server.hpp
    PATH_SUFFIXES websocketpp
)

# ---- nlohmann_json (header-only) ----
find_path(NLOHMANN_JSON_INCLUDE_DIR
    NAMES nlohmann/json.hpp
)

# ---- Boost ----
find_package(Boost REQUIRED COMPONENTS
    locale
    system
)

# ---- Threads ----
find_package(Threads REQUIRED)

# ---- OpenSSL (required by websocketpp ASIO) ----
find_package(OpenSSL REQUIRED)

# ---- SQLite3 ----
find_package(SQLite3 REQUIRED)

# ============================================================
# Source
# ============================================================

set(SOURCES
    main.cpp
)

set(HEADERS
    config.json
)

# ============================================================
# Executable
# ============================================================

add_executable(sqrtalk_server
    ${SOURCES}
)

target_include_directories(sqrtalk_server PRIVATE
    ${WEBSOCKETPP_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${SQLite3_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(sqrtalk_server PRIVATE
    Threads::Threads
    Boost::locale
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    ${CMAKE_DL_LIBS}
)

# ============================================================
# Platform-specific
# ============================================================

if(WIN32)
    target_link_libraries(sqrtalk_server PRIVATE ws2_32)
endif()

# ============================================================
# Install
# ============================================================

install(TARGETS sqrtalk_server DESTINATION bin)
install(FILES config.json DESTINATION bin)
install(DIRECTORY files logs DESTINATION bin OPTIONAL)

# ============================================================
# User-friendly message
# ============================================================

message(STATUS "========================================")
message(STATUS "  SqrtalkServer - IM Chat Server")
message(STATUS "========================================")
message(STATUS "  C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  websocketpp:   ${WEBSOCKETPP_INCLUDE_DIR}")
message(STATUS "  nlohmann_json: ${NLOHMANN_JSON_INCLUDE_DIR}")
message(STATUS "  Boost:         ${Boost_VERSION}")
message(STATUS "========================================")
