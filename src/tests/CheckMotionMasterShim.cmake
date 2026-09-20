set(ALLOWED
    MovePoint Clear MoveIdle MoveChase MoveFollow MoveTargetedHome MoveWaypoint
    MoveRandomAroundPoint MovementExpired MoveJump MoveFlyOrLand Initialize
    MoveFleeing ActiveKind IsChasing IsPatrolling Inhibit Uninhibit)
list(LENGTH ALLOWED ALLOWED_COUNT)
file(GLOB_RECURSE SD3_SOURCES "${SOURCE_ROOT}/src/modules/SD3/*.cpp" "${SOURCE_ROOT}/src/modules/SD3/*.h")
list(LENGTH SD3_SOURCES SD3_COUNT)
if(SD3_COUNT EQUAL 0)
    message(FATAL_ERROR "Motion shim gate found no SD3 sources under ${SOURCE_ROOT}/src/modules/SD3 -- gate is inert")
endif()
set(STRAYS "")
set(SEEN "")
foreach(FILE_PATH IN LISTS SD3_SOURCES)
    file(READ "${FILE_PATH}" CONTENTS)
    string(FIND "${CONTENTS}" "GetMotionMaster()" HAS_CALL)
    if(HAS_CALL EQUAL -1)
        continue()
    endif()
    string(REGEX MATCHALL "GetMotionMaster\\(\\)->[A-Za-z_][A-Za-z0-9_]*" CALLS "${CONTENTS}")
    foreach(CALL IN LISTS CALLS)
        string(REGEX REPLACE "GetMotionMaster\\(\\)->" "" NAME "${CALL}")
        list(APPEND SEEN "${NAME}")
        list(FIND ALLOWED "${NAME}" AT)
        if(AT EQUAL -1)
            file(RELATIVE_PATH REL "${SOURCE_ROOT}" "${FILE_PATH}")
            list(APPEND STRAYS "${REL}: ${NAME}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES STRAYS)
if(STRAYS)
    string(REPLACE ";" "\n  " PRETTY "${STRAYS}")
    message(FATAL_ERROR
        "MotionMaster entry point outside the shim promise:\n  ${PRETTY}\n\n"
        "src/modules/SD3 may call only the facade entry points listed in\n"
        "src/tests/CheckMotionMasterShim.cmake (design v2 section 9). Either route the\n"
        "script through one of them or widen the list in the same commit, with the\n"
        "README in src/game/MotionGenerators updated.")
endif()
list(REMOVE_DUPLICATES SEEN)
list(LENGTH SEEN SEEN_COUNT)
message(STATUS "motion shim: ${ALLOWED_COUNT} entry points, no strays (${SEEN_COUNT} distinct in SD3)")
