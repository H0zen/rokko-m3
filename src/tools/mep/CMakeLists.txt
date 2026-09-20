set(SRC_GRP_CLIENT
    client/AdtParser.cpp
    client/AdtParser.hpp
    client/ChunkReaders.hpp
    client/IMpqArchive.hpp
    client/M2Parser.cpp
    client/M2Parser.hpp
    client/WdtParser.cpp
    client/WdtParser.hpp
    client/WmoParser.cpp
    client/WmoParser.hpp
)
source_group("client" FILES ${SRC_GRP_CLIENT})

add_library(extractor_client STATIC ${SRC_GRP_CLIENT})

target_include_directories(extractor_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/client
)

target_link_libraries(extractor_client
    PUBLIC
        terrain
)

set_target_properties(extractor_client PROPERTIES FOLDER "tools")

set(SRC_GRP_NAV
    nav/NavMeshBuilder.cpp
    nav/NavMeshBuilder.hpp
)
source_group("nav" FILES ${SRC_GRP_NAV})

set(SRC_GRP_DATA
    client/ModelLoaders.cpp
    client/ModelLoaders.hpp
    client/MpqTileSource.cpp
    client/MpqTileSource.hpp
    stores/LiquidTypeStore.hpp
    stores/MapDbcStore.hpp
    stores/MpqDbcLoader.hpp
    ${CMAKE_SOURCE_DIR}/src/shared/DataStores/DBCFileLoader.cpp
)
source_group("data" FILES ${SRC_GRP_DATA})

add_library(extractor_data STATIC ${SRC_GRP_DATA} ${SRC_GRP_NAV})

target_include_directories(extractor_data
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src/shared
        ${CMAKE_SOURCE_DIR}/src/game
        ${CMAKE_SOURCE_DIR}/src/game/WorldHandlers
)

target_link_libraries(extractor_data
    PUBLIC
        extractor_client
        RecastNavigation::Recast
        RecastNavigation::Detour
)

set_target_properties(extractor_data PROPERTIES FOLDER "tools")

add_library(extractor_mpq STATIC
        client/StormLibArchive.cpp
        client/StormLibArchive.hpp
    )

target_link_libraries(extractor_mpq
    PUBLIC
        extractor_data
    PRIVATE
        stormlib
)

set_target_properties(extractor_mpq PROPERTIES FOLDER "tools")

add_library(keymint STATIC KeyMint.cpp KeyMint.h)

target_include_directories(keymint PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(keymint
    PUBLIC
        proto
        shared
        mangos_crypto
)

set_target_properties(keymint PROPERTIES FOLDER "tools")

add_library(client_patcher STATIC
    patch/PeImage.cpp
    patch/ClientFile.cpp
    patch/Patch.cpp
    patch/SecretFile.cpp
    patch/Targets.cpp
)

target_include_directories(client_patcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/patch)
target_link_libraries(client_patcher PUBLIC mangos_crypto)

if(MSVC)
    target_compile_options(client_patcher PRIVATE /W4 /permissive-)
else()
    target_compile_options(client_patcher PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(client_patcher PROPERTIES FOLDER "tools")

add_executable(mep
    mep.cpp
    Extractor.cpp
    ExtractorConsole.cpp
    ExtractorConsole.hpp
    MintMain.cpp
    patch/PatchMain.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/Console/ConsoleUI.cpp
    ${CMAKE_SOURCE_DIR}/src/shared/Console/Terminal.cpp
    $<$<BOOL:${WIN32}>:${CMAKE_SOURCE_DIR}/cmake/win/VersionInfo.rc>
)

target_include_directories(mep
    PRIVATE ${CMAKE_SOURCE_DIR}/src/shared/Console)

target_compile_definitions(mep
    PRIVATE MANGOS_CLIENT_NAME="${MANGOS_CLIENT_NAME}")

target_link_libraries(mep
    PRIVATE
        extractor_mpq
        keymint
        client_patcher
        Threads::Threads
        $<$<BOOL:${WIN32}>:shell32>
        $<$<BOOL:${WIN32}>:ole32>
)

if(WIN32)
    target_include_directories(mep PRIVATE ${CMAKE_BINARY_DIR}/src/shared)
    target_compile_definitions(mep
        PRIVATE
            PRODUCT_ICON="${CMAKE_CURRENT_SOURCE_DIR}/mep.ico"
            PRODUCT_FILE_NAME="mep.exe"
            PRODUCT_FILE_DESCRIPTION="${MANGOS_PKG} extractor and patcher")
    if(TARGET revision_data)
        add_dependencies(mep revision_data)
    endif()
endif()

set_target_properties(mep PROPERTIES FOLDER "tools")
install(TARGETS mep DESTINATION ${BIN_DIR}/tools)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/vessels.txt
              ${CMAKE_CURRENT_SOURCE_DIR}/offmesh.txt
        DESTINATION ${BIN_DIR}/tools)

add_custom_command(TARGET mep POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/vessels.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/offmesh.txt
        $<TARGET_FILE_DIR:mep>)

if(WIN32)
    add_executable(mep-gui WIN32
        ExtractorGui.cpp
        ${CMAKE_SOURCE_DIR}/cmake/win/VersionInfo.rc)
    target_link_libraries(mep-gui PRIVATE shell32 ole32 oleaut32 comctl32 comdlg32)
    target_include_directories(mep-gui PRIVATE ${CMAKE_BINARY_DIR}/src/shared)
    if(TARGET revision_data)
        add_dependencies(mep-gui revision_data)
    endif()
    target_compile_definitions(mep-gui
        PRIVATE
            MANGOS_CLIENT_NAME="${MANGOS_CLIENT_NAME}"
            PRODUCT_ICON="${CMAKE_CURRENT_SOURCE_DIR}/mep.ico"
            PRODUCT_LOGO="${CMAKE_CURRENT_SOURCE_DIR}/mangos-tools.jpg"
            PRODUCT_FILE_NAME="mep-gui.exe"
            PRODUCT_FILE_DESCRIPTION="${MANGOS_PKG} extractor and patcher")
    set_target_properties(mep-gui PROPERTIES FOLDER "tools")
    install(TARGETS mep-gui DESTINATION ${BIN_DIR}/tools)
endif()
