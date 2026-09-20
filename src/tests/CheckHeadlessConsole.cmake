set(CLI_SERVICE "${SOURCE_ROOT}/src/mangosd/CliService.cpp")
if(NOT EXISTS "${CLI_SERVICE}")
    message(FATAL_ERROR "Console reader missing: ${CLI_SERVICE}")
endif()

file(STRINGS "${CLI_SERVICE}" CLI_LINES)

set(SAW_EOF OFF)
set(GUARD_DISTANCE 99)          # lines since the last terminal check
set(STOP_CALLS 0)
set(UNGUARDED "")
set(LINE_NO 0)

foreach(LINE IN LISTS CLI_LINES)
    math(EXPR LINE_NO "${LINE_NO} + 1")

    string(REGEX REPLACE "//.*$" "" CODE "${LINE}")

    if(CODE MATCHES "feof[ \t]*\\([ \t]*stdin[ \t]*\\)")
        set(SAW_EOF ON)
    endif()

    if(CODE MATCHES "StdinIsTerminal[ \t]*\\(")
        set(GUARD_DISTANCE 0)
    else()
        math(EXPR GUARD_DISTANCE "${GUARD_DISTANCE} + 1")
    endif()

    if(CODE MATCHES "World::StopNow[ \t]*\\(")
        math(EXPR STOP_CALLS "${STOP_CALLS} + 1")
        if(GUARD_DISTANCE GREATER 6)
            list(APPEND UNGUARDED "${LINE_NO}")
        endif()
    endif()
endforeach()

if(NOT SAW_EOF)
    message(FATAL_ERROR
        "CliService.cpp no longer tests feof(stdin).\n"
        "If the console stopped reading stdin this gate is obsolete; if it still\n"
        "reads stdin, the end-of-input case has gone unhandled.")
endif()

if(STOP_CALLS EQUAL 0)
    message(FATAL_ERROR
        "CliService.cpp no longer calls World::StopNow().\n"
        "Ctrl-D at a terminal must still stop the server. Update this gate only\n"
        "together with whatever replaced that call.")
endif()

if(UNGUARDED)
    string(REPLACE ";" ", " PRETTY "${UNGUARDED}")
    message(FATAL_ERROR
        "Unguarded World::StopNow() in CliService.cpp at line(s): ${PRETTY}\n\n"
        "End of input on stdin may only stop the world when stdin is a terminal,\n"
        "so the call has to sit inside an if (StdinIsTerminal()) block. Without\n"
        "it every headless host -- systemd, Docker, any service manager that\n"
        "passes /dev/null -- shuts the world down a second after it starts, and\n"
        "does it cleanly enough that nothing reports an error.")
endif()

message(STATUS "headless console: World::StopNow guarded by a terminal check")
