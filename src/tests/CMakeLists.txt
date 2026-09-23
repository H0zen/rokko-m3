set(SRC_GRP_TESTS
    main.cpp
    TestHarness.h
    BigNumberTest.cpp
    Utf8Test.cpp
    ByteBufferTest.cpp
    CalendarTimeTest.cpp
    BareMapTest.cpp
    LFGPacketsTest.cpp
    LFGEmpowermentTest.cpp
    LFGLockReasonTest.cpp
    LFGRoleAssignmentTest.cpp
    LFGProposalLogicTest.cpp
    LFGBootLogicTest.cpp
    LFGDungeonResolutionTest.cpp
    LFGRewardLogicTest.cpp
    LFGCallToArmsLogicTest.cpp
    SessionMailboxTest.cpp
    SessionProtocolPolicyTest.cpp
    OpcodeSlotsTest.cpp
    OpcodeValuesTest.cpp
    MovementCodecTest.cpp
    MovementParityTest.cpp
    ServerRoutingTest.cpp
    SessionLinksTest.cpp
    RedirectRegistryTest.cpp
    RedirectStreamTest.cpp
    ClientFramingTest.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Framing.cpp
    PeerTest.cpp
    ReplayTest.cpp
    GoldenCaptureTest.cpp
    FamilyCodecTest.cpp
    MotionMatrixTest.cpp
    PendingChangesTest.cpp
    TimeBaseTest.cpp
    MotionWritersTest.cpp
    MotionStateTest.cpp
    HarnessTest.cpp
    WorldClockTest.cpp
    RNGSeedTest.cpp
    MotionAuthorityTest.cpp
    MotionArbiterTest.cpp
    MotionMobilityTest.cpp
    MotionBehaviourTest.cpp
    MotionTaxiTest.cpp
    TargetKinematicsTest.cpp
    JumpArcTest.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/TimeSync.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Walker.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Control.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/AckEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Replay.cpp
    GlobalWmoEvictionTest.cpp
    ConnectToTest.cpp
    DatabaseConcurrencyTest.cpp
    NullDatabaseTest.cpp
    GameLinkTest.cpp
    AuthCryptTest.cpp
    ClientParserTest.cpp
    TerrainModelTest.cpp
    TileSerializerTest.cpp
    ModelMapTest.cpp
    NavBinningTest.cpp
    DynamicCollisionTest.cpp
    PlacementTest.cpp
    ByteBufferStressTest.cpp
    CodecStressTest.cpp
    PacketCodecTest.cpp
)

if(WITH_NET_TESTS)
    list(APPEND SRC_GRP_TESTS GracefulCloseTest.cpp)
    list(APPEND SRC_GRP_TESTS NetworkStressTest.cpp)
endif()

if(WITH_CRYPTO_TESTS)
    list(APPEND SRC_GRP_TESTS
        Srp6Test.cpp
        CryptoGoldenTest.cpp
        CryptoGoldenVectors.h
        CryptoHashTest.cpp
        CryptoRandomTest.cpp
        CryptoBigIntTest.cpp
        BigIntVectors.h
        CryptoMontgomeryTest.cpp
        CryptoRsaTest.cpp
        RsaTestKey.h
        CryptoPrimeTest.cpp
        PrimeVectors.h
        CryptoOracleTest.cpp
        CryptoStressTest.cpp
        CryptoBench.cpp
        ${CMAKE_SOURCE_DIR}/src/realmd/Auth/Srp6.cpp)
endif()

source_group("tests" FILES ${SRC_GRP_TESTS})

add_executable(mangos_tests ${SRC_GRP_TESTS})

target_include_directories(mangos_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/game/movement
        ${CMAKE_SOURCE_DIR}/src/realmd
        ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src)

target_compile_definitions(mangos_tests PRIVATE MANGOS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens")

target_link_libraries(mangos_tests
    PRIVATE
        game
        motion
        proto
        shared_db
        mangos_crypto
        extractor_client
        extractor_data
        Threads::Threads
)

# Decoupling D1 (server #80): the test binary links the game library. Every decoupling PR's
# tests stand on this; a tidy-up that drops the link takes them all out with it. Read the real
# link set, not the text of this file.
get_target_property(MANGOS_TESTS_LINKS mangos_tests LINK_LIBRARIES)
if(NOT "game" IN_LIST MANGOS_TESTS_LINKS)
    message(FATAL_ERROR "mangos_tests no longer links the game library (decoupling D1, server #80)")
endif()

if(WITH_CRYPTO_TESTS)
    target_compile_definitions(mangos_tests PRIVATE MANGOS_CRYPTO_TESTS=1)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT MSVC)
        add_subdirectory(oracle)
        target_link_libraries(mangos_tests PRIVATE s2n_bignum_oracle)
        target_compile_definitions(mangos_tests PRIVATE MANGOS_CRYPTO_ORACLE=1)
    endif()
endif()

add_test(NAME mangos_tests COMMAND mangos_tests)

add_test(NAME crypto_asm_twin
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckAsmTwin.cmake)

add_test(NAME proto_boundary
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckProtoBoundary.cmake)

add_test(NAME motion_boundary
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckMotionBoundary.cmake)

add_test(NAME header_reach
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckHeaderReach.cmake)

add_test(NAME warden_dormant_boundary
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckDormantWardenBoundary.cmake)

add_test(NAME crypto_is_ours
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckCryptoIsOurs.cmake)

add_test(NAME build_policy
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckBuildPolicy.cmake)

add_test(NAME spatial_boundary
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckSpatialBoundary.cmake)

add_test(NAME version_sources
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckVersionSources.cmake)

add_test(NAME opcode_denylist
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckOpcodeDenylist.cmake)

add_test(NAME motion_shim
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckMotionMasterShim.cmake)

add_test(NAME motion_blocked_masks
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckBlockedMasks.cmake)

add_test(NAME client_map_id
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckClientMapId.cmake)

add_test(NAME headless_console
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckHeadlessConsole.cmake)

if(WIN32)
    get_filename_component(
        MANGOS_TEST_MYSQL_DLL_DIR "${MySQL_LIBRARY}" DIRECTORY)
    get_filename_component(
        MANGOS_TEST_MYSQL_DLL_NAME "${MySQL_LIBRARY}" NAME)
    string(REPLACE ".lib" ".dll" MANGOS_TEST_MYSQL_DLL_NAME
        "${MANGOS_TEST_MYSQL_DLL_NAME}")
    if(EXISTS "${MANGOS_TEST_MYSQL_DLL_DIR}/${MANGOS_TEST_MYSQL_DLL_NAME}")
        add_custom_command(TARGET mangos_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MANGOS_TEST_MYSQL_DLL_DIR}/${MANGOS_TEST_MYSQL_DLL_NAME}"
                "$<TARGET_FILE_DIR:mangos_tests>")
    endif()
endif()
