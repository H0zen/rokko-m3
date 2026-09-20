set(MOTION_DIR "${SOURCE_ROOT}/src/motion")

if(NOT IS_DIRECTORY "${MOTION_DIR}")
    message(FATAL_ERROR "Motion boundary missing: ${MOTION_DIR}")
endif()

file(GLOB_RECURSE MOTION_SOURCES
    "${MOTION_DIR}/*.h" "${MOTION_DIR}/*.hpp"
    "${MOTION_DIR}/*.cpp" "${MOTION_DIR}/*.cc")

set(FORBIDDEN_PATTERNS
    "#[ \t]*include[ \t]*[\"<](Database/|World\\.h|WorldSession\\.h|AddonHandler\\.h|Warden)"
    "#[ \t]*include[ \t]*[\"<](Unit\\.h|Player\\.h|Creature\\.h|ObjectGuid\\.h|SharedDefines\\.h|Object/|WorldHandlers/|Server/|ChatCommands/|Maps/|MotionGenerators/|movement/)"
    "(^|[^A-Za-z0-9_])(WorldSession|sWorld|sLog|LoginDatabase|CharacterDatabase|WorldDatabase|sAddOnHandler|Warden)([^A-Za-z0-9_]|$)")

set(VIOLATIONS "")

foreach(FILE_PATH IN LISTS MOTION_SOURCES)
    file(STRINGS "${FILE_PATH}" RAW_LINES)

    set(IN_BLOCK OFF)
    set(CODE_ONLY "")

    foreach(LINE IN LISTS RAW_LINES)
        if(IN_BLOCK)
            string(FIND "${LINE}" "*/" CLOSE_AT)
            if(CLOSE_AT EQUAL -1)
                continue()
            endif()
            math(EXPR CLOSE_AT "${CLOSE_AT} + 2")
            string(SUBSTRING "${LINE}" ${CLOSE_AT} -1 LINE)
            set(IN_BLOCK OFF)
        endif()

        string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" " " LINE "${LINE}")

        string(FIND "${LINE}" "/*" OPEN_AT)
        if(NOT OPEN_AT EQUAL -1)
            string(SUBSTRING "${LINE}" 0 ${OPEN_AT} LINE)
            set(IN_BLOCK ON)
        endif()

        string(REGEX REPLACE "//.*$" "" LINE "${LINE}")

        string(APPEND CODE_ONLY "${LINE}\n")
    endforeach()

    foreach(PATTERN IN LISTS FORBIDDEN_PATTERNS)
        if(CODE_ONLY MATCHES "${PATTERN}")
            list(APPEND VIOLATIONS "${FILE_PATH}: ${CMAKE_MATCH_0}")
        endif()
    endforeach()
endforeach()

if(VIOLATIONS)
    string(REPLACE ";" "\n  " VIOLATIONS "${VIOLATIONS}")
    message(FATAL_ERROR "Forbidden game dependency in the motion library:\n  ${VIOLATIONS}")
endif()
