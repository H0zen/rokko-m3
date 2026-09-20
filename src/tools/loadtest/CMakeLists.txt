if(NOT BUILD_TOOLS)
    return()
endif()

add_executable(mangos-loadtest
    src/LoadTest.cpp
    src/Framing.cpp
    src/Framing.hpp
    src/Socket.cpp
    src/Socket.hpp
    src/SyntheticClient.cpp
    src/SyntheticClient.hpp
    src/Peer.hpp
    src/TimeSync.cpp
    src/TimeSync.hpp
    src/Walker.cpp
    src/Walker.hpp
    src/Control.cpp
    src/Control.hpp
    src/AckEngine.cpp
    src/AckEngine.hpp
    src/Replay.hpp
    src/Replay.cpp
)

target_include_directories(mangos-loadtest PRIVATE src)

target_link_libraries(mangos-loadtest
    PRIVATE
        proto
        shared
)

if(MSVC)
    target_compile_options(mangos-loadtest PRIVATE /W4)
else()
    target_compile_options(mangos-loadtest PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_link_libraries(mangos-loadtest PRIVATE ws2_32)
endif()

set_target_properties(mangos-loadtest PROPERTIES FOLDER "tools")

install(TARGETS mangos-loadtest DESTINATION ${BIN_DIR}/tools)
