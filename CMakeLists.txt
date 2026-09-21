cmake_minimum_required(VERSION 3.18 FATAL_ERROR)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(MaNGOS VERSION 0.22.0 LANGUAGES C CXX)

if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure from a separate build "
        "directory, e.g.  cmake -S . -B ../build")
endif()

cmake_policy(SET CMP0028 NEW)
cmake_policy(SET CMP0054 NEW)
cmake_policy(SET CMP0042 NEW)
cmake_policy(SET CMP0048 NEW)
cmake_policy(SET CMP0063 NEW)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set_property(GLOBAL
    PROPERTY USE_FOLDERS ON
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_MANGOSD        "Build the main server"                         ON)
option(BUILD_REALMD         "Build the login server"                        ON)
option(BUILD_TOOLS          "Build the client-data extractor"               ON)
option(SCRIPT_LIB_SD3       "Compile with support for ScriptDev3 scripts"   ON)
option(PCH                  "Enable precompiled headers"                    ON)
option(WITH_IO_URING        "Use the io_uring network backend (Linux, needs liburing)" OFF)
option(WITH_TESTS           "Build the unit tests"                          OFF)
option(WITH_NET_TESTS       "Build the socket tests (slow; never in CI)"    OFF)
option(WITH_CRYPTO_TESTS    "Build the crypto vector and stress tests (CI)" OFF)
option(DEBUG                "Enable debug build (only on non IDEs)"         OFF)
option(WITHOUT_GIT          "Disable Git revision detection"                OFF)
message("")
message(
  "This script builds the MaNGOS server.
  Options that can be used in order to configure the process:
   General:
    CMAKE_INSTALL_PREFIX    Path where the server should be installed to
    BUILD_MANGOSD           Build the main server
    BUILD_REALMD            Build the login server
    BUILD_TOOLS             Build the client-data extractor
    PCH                     Enable use of precompiled headers
    WITH_IO_URING           Use the io_uring network backend instead of epoll (Linux only,
                            requires liburing headers; falls back to epoll if absent)
    WITH_TESTS              Build the unit tests (target: mangos_tests)
    WITH_NET_TESTS          Build the socket tests as well. Disjoint from WITH_TESTS,
                            off by default, and deliberately not run in CI: they bind
                            real sockets and dominate the suite's wall-clock time.
    WITH_CRYPTO_TESTS       Build the crypto vector, golden and stress tests. Off by
                            default: src/shared/Crypto is frozen, and its ~3k lines of
                            vector tables are compile time paid on every local build
                            for code that does not change. CI turns it on.
    DEBUG                   Debug build, only for systems without IDE (Linux, *BSD)
   Scripting engines:
    SCRIPT_LIB_SD3          Compile with support for ScriptDev3 scripts

  To set an option simply type -D<OPTION>=<VALUE> after 'cmake <srcs>'.
  Also, you can specify the generator with -G. See 'cmake --help' for more details
  For example: cmake .. -DCMAKE_INSTALL_PREFIX=/opt/mangos -DSCRIPT_LIB_SD3=0"
)
message("")

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install CACHE PATH "Default install directory" FORCE)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES)
  if(DEBUG)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
  else()
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()
endif()

include(GNUInstallDirs)

set(BIN_DIR ${CMAKE_INSTALL_FULL_BINDIR})
set(CONF_INSTALL_DIR ${CMAKE_INSTALL_FULL_SYSCONFDIR})
set(CONF_RUNTIME_DIR ${CMAKE_INSTALL_FULL_SYSCONFDIR})

if(NOT DATA_RUNTIME_DIR)
    set(DATA_RUNTIME_DIR ${CMAKE_INSTALL_PREFIX}/data)
endif()

if(NOT WITHOUT_GIT)
  find_package(Git QUIET)
endif()

find_package(Threads REQUIRED)
find_package(MySQL   REQUIRED)

find_package(ZLIB  QUIET)
find_package(BZip2 QUIET)

set(MANGOS_USE_IO_URING OFF)
if(WITH_IO_URING AND UNIX AND NOT APPLE)
    find_library(URING_LIBRARY NAMES uring)
    find_path(URING_INCLUDE_DIR NAMES liburing.h)
    if(URING_LIBRARY AND URING_INCLUDE_DIR)
        set(MANGOS_USE_IO_URING ON)
    else()
        message(WARNING "WITH_IO_URING was requested but liburing was not found; "
                        "falling back to the epoll reactor.")
    endif()
endif()

include(${CMAKE_SOURCE_DIR}/cmake/MangosVersion.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/MangosPlatform.cmake)
add_subdirectory(dep)

if(WITH_TESTS OR WITH_NET_TESTS)
    set(BUILD_TESTING ON)
    enable_testing()
endif()

add_subdirectory(src)

include(${CMAKE_SOURCE_DIR}/cmake/MangosStatus.cmake)
