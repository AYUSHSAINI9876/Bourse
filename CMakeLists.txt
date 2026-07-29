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

  # L2 -- in-memory keyspace
  src/cache/value.cpp
  src/cache/eviction.cpp
  src/cache/keyspace.cpp

  # L3 -- command execution
  src/exec/reply.cpp
  src/exec/command_registry.cpp
  src/exec/commands.cpp
  src/exec/pubsub.cpp

  # L5 -- network and protocols
  src/net/poller.cpp
  src/net/event_loop.cpp
  src/net/connection.cpp
  src/net/server.cpp
  src/net/resp_codec.cpp

  # server assembly
  src/server/bourse_server.cpp
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
