set(DENYLIST_FILE "${CMAKE_CURRENT_LIST_DIR}/opcode_denylist.txt")
if(NOT EXISTS "${DENYLIST_FILE}")
    message(FATAL_ERROR "Opcode denylist missing: ${DENYLIST_FILE}")
endif()

file(STRINGS "${DENYLIST_FILE}" DENY_LINES REGEX "^(SMSG|CMSG|MSG)_[A-Z0-9_]+")
set(DENY_NAMES "")
foreach(LINE IN LISTS DENY_LINES)
    if(LINE MATCHES "^((SMSG|CMSG|MSG)_[A-Z0-9_]+)")
        list(APPEND DENY_NAMES "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(LENGTH DENY_NAMES DENY_COUNT)
if(DENY_COUNT EQUAL 0)
    message(FATAL_ERROR "Opcode denylist parsed to zero names -- gate is inert")
endif()

list(JOIN DENY_NAMES "|" DENY_ALT)
set(DENY_PATTERN "(^|[^A-Za-z0-9_])(${DENY_ALT})([^A-Za-z0-9_]|$)")

file(GLOB_RECURSE GAME_SOURCES "${SOURCE_ROOT}/src/game/*.cpp" "${SOURCE_ROOT}/src/game/*.h")
set(VIOLATIONS "")

foreach(FILE_PATH IN LISTS GAME_SOURCES)
    file(READ "${FILE_PATH}" FILE_CONTENTS)
    string(FIND "${FILE_CONTENTS}" "MSG_" FILE_HAS_MSG)
    if(FILE_HAS_MSG EQUAL -1)
        continue()
    endif()

    file(STRINGS "${FILE_PATH}" RAW_LINES)
    set(IN_BLOCK OFF)
    set(LINE_NO 0)
    foreach(LINE IN LISTS RAW_LINES)
        math(EXPR LINE_NO "${LINE_NO} + 1")

        if(IN_BLOCK)
            string(FIND "${LINE}" "*/" CLOSE_AT)
            if(CLOSE_AT EQUAL -1)
                continue()
            endif()
            math(EXPR CLOSE_AT "${CLOSE_AT} + 2")
            string(SUBSTRING "${LINE}" ${CLOSE_AT} -1 LINE)
            set(IN_BLOCK OFF)
        endif()

        string(FIND "${LINE}" "/*" OPEN_AT)
        if(NOT OPEN_AT EQUAL -1)
            string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" " " LINE "${LINE}")

            string(FIND "${LINE}" "/*" OPEN_AT)
            if(NOT OPEN_AT EQUAL -1)
                string(SUBSTRING "${LINE}" 0 ${OPEN_AT} LINE)
                set(IN_BLOCK ON)
            endif()
        endif()

        string(FIND "${LINE}" "//" SLASHES_AT)
        if(NOT SLASHES_AT EQUAL -1)
            string(REGEX REPLACE "//.*$" "" LINE "${LINE}")
        endif()

        string(FIND "${LINE}" "MSG_" HAS_MSG)
        if(HAS_MSG EQUAL -1)
            continue()
        endif()

        if(LINE MATCHES "${DENY_PATTERN}")
            set(HIT_NAME "${CMAKE_MATCH_2}")

            string(STRIP "${LINE}" TRIMMED)
            if(FILE_PATH MATCHES "OpcodeTable\\.cpp$"
               AND TRIMMED MATCHES "^OPCODE[ \t]*\\([ \t]*SMSG_[A-Z0-9_]+[ \t]*,")
            else()
                list(APPEND VIOLATIONS "${FILE_PATH}:${LINE_NO}: ${HIT_NAME}")
            endif()
        endif()
    endforeach()
endforeach()

if(VIOLATIONS)
    string(REPLACE ";" "\n  " PRETTY "${VIOLATIONS}")
    message(FATAL_ERROR
        "Known-wrong opcode value adopted:\n  ${PRETTY}\n\n"
        "These names carry values the 4.3.4 client does not agree with. Using one\n"
        "ships a silent no-op. Correct the value in src/proto/Opcodes.h from the\n"
        "'correct' column of src/tests/opcode_denylist.txt, then remove the name\n"
        "from that file in the same commit.")
endif()

message(STATUS "opcode denylist: ${DENY_COUNT} names, no adoptions")
