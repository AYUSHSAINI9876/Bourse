cmake_minimum_required(VERSION 3.20)

project(bourse
        VERSION 1.0.0
        DESCRIPTION "A from-scratch trading exchange with its own storage engine, cache, and query layer"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CompilerWarnings)
include(Sanitizers)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BOURSE_BUILD_TESTS "Build the unit/integration test suite" ON)
option(BOURSE_BUILD_APPS  "Build the server executable"           ON)
option(BOURSE_WERROR      "Treat warnings as errors"              OFF)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(bourse_core STATIC
  # L0 -- foundation
  src/core/logger.cpp
  src/core/metrics.cpp
  src/core/byte_buffer.cpp
  src/core/socket.cpp
  src/core/file.cpp
  src/core/system_error.cpp

  # L1 -- durable storage
  src/storage/wal.cpp
  src/storage/snapshot.cpp
  src/storage/buffer_pool.cpp
  src/storage/bplus_tree.cpp

  # L2 -- in-memory keyspace
  src/cache/value.cpp
  src/cache/eviction.cpp
  src/cache/keyspace.cpp

  # L2 -- identity. Sits beside the keyspace rather than above it: both net
  # and exec consult it, and it depends on neither.
  src/auth/crypto.cpp
  src/auth/totp.cpp
  src/auth/auth_service.cpp

  # L3 -- command execution
  src/exec/reply.cpp
  src/exec/command_registry.cpp
  src/exec/commands.cpp
  src/exec/exchange_commands.cpp
  src/exec/sql_commands.cpp
  src/exec/auth_commands.cpp
  src/exec/pubsub.cpp

  # L3b -- SQL execution
  src/sql/ast.cpp
  src/sql/lexer.cpp
  src/sql/parser.cpp
  src/sql/executor.cpp
  src/sql/engine.cpp

  # L4 -- matching engine
  src/match/order_book.cpp
  src/match/matching_engine.cpp

  # L5 -- network and protocols
  src/net/poller.cpp
  src/net/event_loop.cpp
  src/net/connection.cpp
  src/net/server.cpp
  src/net/resp_codec.cpp
  src/net/http_codec.cpp
  src/net/router.cpp

  # server assembly
  src/server/bourse_server.cpp
  src/server/rest_api.cpp
)

target_include_directories(bourse_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)
target_link_libraries(bourse_core PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(bourse_core PUBLIC ws2_32 mswsock)
  target_compile_definitions(bourse_core PUBLIC
    BOURSE_PLATFORM_WINDOWS=1 _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(bourse_core PUBLIC BOURSE_PLATFORM_LINUX=1 BOURSE_HAVE_EPOLL=1)
else()
  target_compile_definitions(bourse_core PUBLIC BOURSE_PLATFORM_POSIX=1)
endif()

bourse_set_warnings(bourse_core)
bourse_enable_sanitizers(bourse_core)

# ---------------------------------------------------------------------------
# Embedded dashboard
# ---------------------------------------------------------------------------
# frontend/index.html is compiled into the binary as a string_view, so
# bourse-server is a single self-contained artefact with no asset path to get
# wrong at deploy time and no static-file handler to secure.
#
# It is the same file the CDN serves -- reached across the frontend/backend
# split rather than copied -- so the hosted dashboard and the one baked into
# the binary cannot drift apart. BOURSE_DASHBOARD_SRC is cached so a build
# from somewhere other than the repo (a submodule, a tarball of backend/ only)
# can point it at a different copy rather than failing to configure.
add_executable(bourse-crypto-check apps/crypto_check/main.cpp)
target_link_libraries(bourse-crypto-check PRIVATE bourse_core)
bourse_set_warnings(bourse-crypto-check)
bourse_enable_sanitizers(bourse-crypto-check)

set(BOURSE_DASHBOARD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../frontend/index.html"
    CACHE FILEPATH "Dashboard HTML compiled into the server binary")
if(NOT EXISTS ${BOURSE_DASHBOARD_SRC})
  message(FATAL_ERROR
    "Dashboard asset not found: ${BOURSE_DASHBOARD_SRC}\n"
    "The server embeds frontend/index.html at build time. Configure from a "
    "full checkout, or pass -DBOURSE_DASHBOARD_SRC=<path>.")
endif()
set(BOURSE_DASHBOARD_HDR ${CMAKE_BINARY_DIR}/generated/bourse/dashboard_asset.hpp)
add_custom_command(
  OUTPUT ${BOURSE_DASHBOARD_HDR}
  COMMAND ${CMAKE_COMMAND}
          -DINPUT_FILE=${BOURSE_DASHBOARD_SRC}
          -DOUTPUT_FILE=${BOURSE_DASHBOARD_HDR}
          -DVARIABLE_NAME=kDashboardHtml
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAsset.cmake
  DEPENDS ${BOURSE_DASHBOARD_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAsset.cmake
  COMMENT "Embedding frontend/index.html"
  VERBATIM)
add_custom_target(bourse_dashboard_asset DEPENDS ${BOURSE_DASHBOARD_HDR})
add_dependencies(bourse_core bourse_dashboard_asset)
target_sources(bourse_core PRIVATE ${BOURSE_DASHBOARD_HDR})
target_include_directories(bourse_core PUBLIC $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
if(BOURSE_BUILD_APPS)
  add_executable(bourse-server apps/bourse_server/main.cpp)
  target_link_libraries(bourse-server PRIVATE bourse_core)
  bourse_set_warnings(bourse-server)
  bourse_enable_sanitizers(bourse-server)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BOURSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  Bourse ${PROJECT_VERSION}")
message(STATUS "  ---------------------------------")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Platform      : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Tests         : ${BOURSE_BUILD_TESTS}")
message(STATUS "  Sanitizer     : ${BOURSE_SANITIZER}")
message(STATUS "")
