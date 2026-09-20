set(MANGOS_PKG "Mangos Three")

set(MANGOS_WORLD_VER 2026092000)
set(MANGOS_REALM_VER 2026092000)
set(MANGOS_AHBOT_VER 2026092000)

set(MANGOS_REALMD_DB_VERSION   "22")
set(MANGOS_REALMD_DB_STRUCTURE "5")
set(MANGOS_REALMD_DB_CONTENT   "1")
set(MANGOS_REALMD_DB_DESCRIPT  "Remove_Playerbots")

set(MANGOS_CHAR_DB_VERSION   "22")
set(MANGOS_CHAR_DB_STRUCTURE "10")
set(MANGOS_CHAR_DB_CONTENT   "1")
set(MANGOS_CHAR_DB_DESCRIPT  "Remove_Playerbots")

set(MANGOS_WORLD_DB_VERSION   "22")
set(MANGOS_WORLD_DB_STRUCTURE "10")
set(MANGOS_WORLD_DB_CONTENT   "3")
set(MANGOS_WORLD_DB_DESCRIPT  "Pinfo_Both_Latencies")

set(MANGOS_CLIENT_BUILD   15595)
set(MANGOS_CLIENT_VERSION "4.3.4")
set(MANGOS_CLIENT_NAME    "Cataclysm ${MANGOS_CLIENT_VERSION}")

if(NOT BUILDDIR)
    set(BUILDDIR ${CMAKE_BINARY_DIR})
endif()

set(rev_hash   "unknown")
set(rev_date   "1970-01-01 00:00:00 +0000")
set(rev_branch "Archived")
string(TIMESTAMP rev_year "%Y" UTC)

if(NOT WITHOUT_GIT AND GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --long --match init --dirty=+ --abbrev=12 --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE rev_info OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    if(rev_info)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" show -s --format=%ci
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE rev_date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE rev_branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

        string(REGEX REPLACE "init-|[0-9]+-g" "" rev_hash "${rev_info}")
        string(REGEX MATCH "^[0-9]+" rev_year "${rev_date}")
    else()
        message(STATUS "No repository signature (try: git fetch -t); reporting "
                       "\"${rev_hash} ${rev_date} (${rev_branch} branch)\"")
    endif()
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/src/shared/BuildInfo.h.in"
    "${BUILDDIR}/src/shared/BuildInfo.h"
    @ONLY
)
