# Decoupling D3 (server #77): the god headers do not reach the layers they forward-declare.
# For each rule "header|forbidden,forbidden,..." the transitive includes of the header are
# walked: an include resolves relative to the including file first, then by basename against
# every directory under src/game, src/shared, src/proto and src/motion -- the same resolution
# the counter tool (src/tests/tools/include_reach.py) uses, which is stricter than the compiler
# and so cannot be bypassed by an include the compiler would not find. The walk must not touch
# a forbidden header. Object/Unit.h has one more rule: nothing under src/motion but Mobility.h.
# The PCH is never read: with it on, every translation unit sees the world and this check
# would mean nothing.
# Run standalone (-P), this script sees none of the top-level project's policies: without this,
# CMP0057 stays OLD and IN_LIST is parsed as plain text instead of the if() operator it is used
# as below. The project requires CMake >= 3.18, so that is also the floor here.
cmake_minimum_required(VERSION 3.18)

set(GAME_DIR "${SOURCE_ROOT}/src/game")
set(MOTION_DIR "${SOURCE_ROOT}/src/motion")
get_filename_component(MOTION_DIR "${MOTION_DIR}" REALPATH)

set(INCLUDE_DIRS "")
foreach(TOP IN ITEMS "${GAME_DIR}" "${SOURCE_ROOT}/src/shared" "${SOURCE_ROOT}/src/proto" "${MOTION_DIR}")
    list(APPEND INCLUDE_DIRS "${TOP}")
    file(GLOB_RECURSE SUBDIRS LIST_DIRECTORIES true "${TOP}/*")
    foreach(ENTRY IN LISTS SUBDIRS)
        if(IS_DIRECTORY "${ENTRY}")
            list(APPEND INCLUDE_DIRS "${ENTRY}")
        endif()
    endforeach()
endforeach()

# Each entry: "<header relative to src/game>|<forbidden suffix>,<forbidden suffix>,..."
# Commas, not semicolons: a semicolon inside a quoted argument splits the CMake list.
# A forbidden suffix matches any reached path that ends with "/<suffix>".
# Database/DatabaseEnv.h and WorldHandlers/ScriptMgr.h join this rule in D3 PR 3, when
# AchievementMgr and BattleGround.h leave Player.h.
set(REACH_RULES
    "Object/Player.h|Object/GMTicketMgr.h,Object/Bag.h,Server/DBCStores.h,WorldHandlers/NPCHandler.h,WorldHandlers/Chat.h,Server/WorldSession.h"
    "Object/Unit.h|")
set(MOTION_ONLY_HEADER "")
set(MOTION_ALLOWED "Mobility.h")

function(resolve_include INCLUDE FROM_DIR OUT_VAR)
    set(CANDIDATE "${FROM_DIR}/${INCLUDE}")
    if(EXISTS "${CANDIDATE}" AND NOT IS_DIRECTORY "${CANDIDATE}")
        get_filename_component(CANDIDATE "${CANDIDATE}" REALPATH)
        set(${OUT_VAR} "${CANDIDATE}" PARENT_SCOPE)
        return()
    endif()
    get_filename_component(BASENAME "${INCLUDE}" NAME)
    foreach(DIR IN LISTS INCLUDE_DIRS)
        foreach(NAME IN ITEMS "${INCLUDE}" "${BASENAME}")
            set(CANDIDATE "${DIR}/${NAME}")
            if(EXISTS "${CANDIDATE}" AND NOT IS_DIRECTORY "${CANDIDATE}")
                get_filename_component(CANDIDATE "${CANDIDATE}" REALPATH)
                set(${OUT_VAR} "${CANDIDATE}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
    set(${OUT_VAR} "" PARENT_SCOPE)
endfunction()

function(walk_includes START OUT_VAR)
    set(QUEUE "${START}")
    set(SEEN "")
    while(QUEUE)
        list(POP_FRONT QUEUE CURRENT)
        file(STRINGS "${CURRENT}" LINES REGEX "^[ \t]*#[ \t]*include")
        get_filename_component(CURRENT_DIR "${CURRENT}" DIRECTORY)
        foreach(LINE IN LISTS LINES)
            string(REGEX MATCH "[\"<]([^\">]+)[\">]" _ "${LINE}")
            set(INCLUDE "${CMAKE_MATCH_1}")
            if(NOT INCLUDE)
                continue()
            endif()
            resolve_include("${INCLUDE}" "${CURRENT_DIR}" RESOLVED)
            if(RESOLVED AND NOT RESOLVED IN_LIST SEEN)
                list(APPEND SEEN "${RESOLVED}")
                list(APPEND QUEUE "${RESOLVED}")
            endif()
        endforeach()
    endwhile()
    set(${OUT_VAR} "${SEEN}" PARENT_SCOPE)
endfunction()

# Parser self-test (decoupling D3, F1): a rule with three forbidden entries must yield three.
string(REPLACE "|" ";" SELF_TEST "x.h|a.h,b.h,c.h")
list(GET SELF_TEST 1 SELF_TEST_FORBIDDEN)
string(REPLACE "," ";" SELF_TEST_FORBIDDEN "${SELF_TEST_FORBIDDEN}")
list(LENGTH SELF_TEST_FORBIDDEN SELF_TEST_COUNT)
if(NOT SELF_TEST_COUNT EQUAL 3)
    message(FATAL_ERROR "Header reach: the rule parser split 'a.h,b.h,c.h' into ${SELF_TEST_COUNT} entries, not 3")
endif()

set(VIOLATIONS "")
foreach(RULE IN LISTS REACH_RULES)
    string(REPLACE "|" ";" PARTS "${RULE}")
    list(GET PARTS 0 HEADER)
    list(LENGTH PARTS PART_COUNT)
    set(FORBIDDEN "")
    if(PART_COUNT GREATER 1)
        list(GET PARTS 1 FORBIDDEN)
        string(REPLACE "," ";" FORBIDDEN "${FORBIDDEN}")
    endif()
    get_filename_component(START "${GAME_DIR}/${HEADER}" REALPATH)
    if(NOT EXISTS "${START}")
        message(FATAL_ERROR "Header reach: ${HEADER} does not exist")
    endif()
    walk_includes("${START}" REACHED)
    list(LENGTH REACHED REACHED_COUNT)
    message(STATUS "header reach: ${HEADER} -> ${REACHED_COUNT} headers")
    foreach(SUFFIX IN LISTS FORBIDDEN)
        if(NOT SUFFIX)
            continue()
        endif()
        string(REPLACE "." "\\." SUFFIX_RE "${SUFFIX}")
        foreach(PATH IN LISTS REACHED)
            string(REGEX MATCH "(^|/)${SUFFIX_RE}$" HIT "${PATH}")
            if(HIT)
                list(APPEND VIOLATIONS "${HEADER} reaches ${SUFFIX} (${PATH})")
            endif()
        endforeach()
    endforeach()
    if(HEADER STREQUAL MOTION_ONLY_HEADER)
        foreach(PATH IN LISTS REACHED)
            string(FIND "${PATH}" "${MOTION_DIR}/" AT)
            if(AT EQUAL 0)
                get_filename_component(NAME "${PATH}" NAME)
                if(NOT NAME IN_LIST MOTION_ALLOWED)
                    list(APPEND VIOLATIONS "${HEADER} reaches the movement kernel header ${NAME}; only ${MOTION_ALLOWED} is allowed")
                endif()
            endif()
        endforeach()
    endif()
endforeach()

if(VIOLATIONS)
    string(REPLACE ";" "\n  " REPORT "${VIOLATIONS}")
    message(FATAL_ERROR
        "A god header reaches a layer it must only forward-declare (decoupling D3, server #77):\n  ${REPORT}\n"
        "Forward-declare the type and move the include to the .cpp that needs it.")
endif()
