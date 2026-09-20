file(GLOB SRC_GRP_AHBOT AuctionHouseBot/*.cpp AuctionHouseBot/*.h)
source_group("AhBot" FILES ${SRC_GRP_AHBOT})

file(GLOB SRC_GRP_BATTLEGROUND BattleGround/*.cpp BattleGround/*.h)
source_group("BattleGround" FILES ${SRC_GRP_BATTLEGROUND})

file(GLOB SRC_GRP_CHAT_COMMANDS ChatCommands/*.cpp ChatCommands/*.h)
source_group("Chat Commands" FILES ${SRC_GRP_CHAT_COMMANDS})

file(GLOB SRC_GRP_MAP_THREAD Maps/*.cpp Maps/*.h)
source_group("Map Threading" FILES ${SRC_GRP_MAP_THREAD})

file(GLOB SRC_GRP_HARNESS Harness/*.cpp Harness/*.h)
source_group("Harness" FILES ${SRC_GRP_HARNESS})

file(GLOB SRC_GRP_MOTION_GEN MotionGenerators/*.cpp MotionGenerators/*.h)
source_group("Motion generators" FILES ${SRC_GRP_MOTION_GEN})

file(GLOB SRC_GRP_MOVEMENT movement/*.cpp movement/*.h)
source_group("Movement" FILES ${SRC_GRP_MOVEMENT})

file(GLOB SRC_GRP_OBJECT Object/*.cpp Object/*.h)
source_group("Object" FILES ${SRC_GRP_OBJECT})

file(GLOB SRC_GRP_OUTDOOR_PVP OutdoorPvP/*.cpp OutdoorPvP/*.h)
source_group("Outdoor PvP" FILES ${SRC_GRP_OUTDOOR_PVP})

file(GLOB SRC_GRP_REFERENCES References/*.cpp References/*.h)
source_group("References" FILES ${SRC_GRP_REFERENCES})

file(GLOB SRC_GRP_SERVER Server/*.cpp Server/*.h)
source_group("Server" FILES ${SRC_GRP_SERVER})

file(GLOB SRC_GRP_TIME Time/*.cpp Time/*.h)
source_group("Time" FILES ${SRC_GRP_TIME})

file(GLOB SRC_GRP_TOOL Tools/*.cpp Tools/*.h)
source_group("Tool" FILES ${SRC_GRP_TOOL})

file(GLOB SRC_GRP_WORLD_HANDLERS WorldHandlers/*.cpp WorldHandlers/*.h)
source_group("World\\Handlers" FILES ${SRC_GRP_WORLD_HANDLERS})

configure_file(AuctionHouseBot/ahbot.conf.dist.in ${CMAKE_CURRENT_BINARY_DIR}/AuctionHouseBot/ahbot.conf.dist)

add_library(game STATIC
    ${SRC_GRP_AHBOT}
    ${SRC_GRP_BATTLEGROUND}
    ${SRC_GRP_CHAT_COMMANDS}
    ${SRC_GRP_MAP_THREAD}
    ${SRC_GRP_HARNESS}
    ${SRC_GRP_MOTION_GEN}
    ${SRC_GRP_MOVEMENT}
    ${SRC_GRP_OBJECT}
    ${SRC_GRP_OUTDOOR_PVP}
    ${SRC_GRP_REFERENCES}
    ${SRC_GRP_SERVER}
    ${SRC_GRP_TIME}
    ${SRC_GRP_TOOL}
    ${SRC_GRP_WORLD_HANDLERS}
)

target_include_directories(game
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        AuctionHouseBot
        BattleGround
        ChatCommands
        Harness
        Maps
        MotionGenerators
        Object
        OutdoorPvP
        References
        Server
        Time
        Tools
        WorldHandlers
)

target_compile_definitions(game
    PUBLIC
        $<$<BOOL:${SCRIPT_LIB_SD3}>:ENABLE_SD3>
)

target_link_libraries(game
    PUBLIC
        terrain
        geometry
        shared_db
        proto
        motion
        RecastNavigation::Detour
        ZLIB::ZLIB
        $<$<BOOL:${SCRIPT_LIB_SD3}>:mangosscript>
)

if(PCH)
    add_cxx_pch(game pchdef.h pchdef.cpp)
endif()

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/AuctionHouseBot/ahbot.conf.dist
    DESTINATION ${CONF_INSTALL_DIR}
)
