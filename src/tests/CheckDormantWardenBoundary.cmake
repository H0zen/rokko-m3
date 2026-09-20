if(NOT DEFINED SOURCE_ROOT)
    get_filename_component(SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

set(OPCODE_TABLE "${SOURCE_ROOT}/src/game/Server/OpcodeTable.cpp")
set(SESSION_HEADER "${SOURCE_ROOT}/src/game/Server/WorldSession.h")
set(HANDLER_SOURCE "${SOURCE_ROOT}/src/game/WorldHandlers/WardenHandler.cpp")

foreach(REQUIRED_PATH OPCODE_TABLE SESSION_HEADER HANDLER_SOURCE)
    if(NOT EXISTS "${${REQUIRED_PATH}}")
        message(FATAL_ERROR
            "Dormant Warden boundary artifact is missing: ${${REQUIRED_PATH}}")
    endif()
endforeach()

file(STRINGS "${OPCODE_TABLE}" OPCODE_LINES)
set(CMSG_COUNT 0)
set(SMSG_COUNT 0)
foreach(LINE IN LISTS OPCODE_LINES)
    if(LINE MATCHES "^[ \t]*//")
        continue()
    endif()

    string(REGEX REPLACE "[ \t]" "" COMPACT_LINE "${LINE}")
    if(COMPACT_LINE STREQUAL
        "OPCODE(CMSG_WARDEN_DATA,STATUS_AUTHED,PROCESS_THREADUNSAFE,&WorldSession::HandleWardenDataOpcode);")
        math(EXPR CMSG_COUNT "${CMSG_COUNT} + 1")
    elseif(COMPACT_LINE STREQUAL
        "OPCODE(SMSG_WARDEN_DATA,STATUS_NEVER,PROCESS_INPLACE,&WorldSession::Handle_ServerSide);")
        math(EXPR SMSG_COUNT "${SMSG_COUNT} + 1")
    endif()
endforeach()

if(NOT CMSG_COUNT EQUAL 1)
    message(FATAL_ERROR
        "CMSG_WARDEN_DATA must have one authenticated, thread-unsafe drain registration")
endif()
if(NOT SMSG_COUNT EQUAL 1)
    message(FATAL_ERROR
        "SMSG_WARDEN_DATA must have one dormant server-side registration")
endif()

file(READ "${SESSION_HEADER}" SESSION_TEXT)
string(FIND "${SESSION_TEXT}"
    "void HandleWardenDataOpcode(WorldPacket& recv_data);"
    SESSION_DECLARATION_POSITION)
if(SESSION_DECLARATION_POSITION EQUAL -1)
    message(FATAL_ERROR
        "WorldSession must declare the Warden drain handler")
endif()

file(READ "${HANDLER_SOURCE}" HANDLER_TEXT)

string(REGEX MATCH
    "void[ 	
]*WorldSession::HandleWardenDataOpcode[^{]*{([^}]*)}"
    UNUSED "${HANDLER_TEXT}")
set(HANDLER_BODY "${CMAKE_MATCH_1}")
string(REGEX REPLACE "[ 	
]" "" HANDLER_BODY "${HANDLER_BODY}")

if(NOT HANDLER_BODY STREQUAL "recv_data.rfinish();")
    message(FATAL_ERROR
        "Warden handler must drain the complete packet and do nothing else, but its body is: ${HANDLER_BODY}")
endif()

foreach(FORBIDDEN_PATTERN
    "DEBUG_LOG\\(" "ERROR_LOG\\(" "sLog" "operator>>" ">>"
    "\\.read" "Decrypt" "_warden")
    if(HANDLER_TEXT MATCHES "${FORBIDDEN_PATTERN}")
        message(FATAL_ERROR
            "Dormant Warden handler contains parsing, logging, or runtime state: ${FORBIDDEN_PATTERN}")
    endif()
endforeach()

message(STATUS
    "Dormant Warden opcodes retain an authenticated drain-only client boundary")
