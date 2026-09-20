# SPDX-License-Identifier: GPL-3.0-or-later
#
# MaNGOS is a full featured server for World of Warcraft, supporting
# the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
#
# Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# =============================================================================
# Unit tests. -DWITH_TESTS=1 builds them; -DWITH_NET_TESTS=1 adds the socket tests.
#
# Scope is deliberately narrow: the pieces that are pure functions over bytes and
# whose failure modes are invisible in ordinary use -- packet reassembly under
# fragmentation, and a zero-padding bug that shows about one login in 256.
#
# THE TWO SWITCHES ARE DISJOINT. The socket tests bind real listeners and cost more
# wall-clock than the whole rest of the suite, so WITH_TESTS never implies them. They
# are not run in CI. Ask for them by name when a change touches the reactor.
#
# No test framework is vendored; see TestHarness.h.
# =============================================================================

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
    # The load-test client's framing, compiled in so the test can check it against
    # proto's own codec rather than against a copy of itself. Its asymmetry with
    # the server's framing has misled this project twice.
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Framing.cpp
    # The protocol peer's pure units (movement P0-B), compiled in like Framing.cpp.
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
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/TimeSync.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Walker.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Control.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/AckEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src/Replay.cpp
    GlobalWmoEvictionTest.cpp
    ConnectToTest.cpp
    DatabaseConcurrencyTest.cpp
    AuthCryptTest.cpp
    ClientParserTest.cpp
    TerrainModelTest.cpp
    TileSerializerTest.cpp
    ModelMapTest.cpp
    NavBinningTest.cpp
    DynamicCollisionTest.cpp
    PlacementTest.cpp
    # Compiled in, not linked from `game`: game.lib pulls the whole server, down to the
    # database globals that only mangosd defines. These three know nothing of it.
    ${CMAKE_SOURCE_DIR}/src/game/WorldHandlers/DynamicCollision.cpp
    ${CMAKE_SOURCE_DIR}/src/game/WorldHandlers/GameObjectModel.cpp
    ${CMAKE_SOURCE_DIR}/src/game/Server/SessionMailbox.cpp
    ${CMAKE_SOURCE_DIR}/src/game/Harness/Timeline.cpp
    ByteBufferStressTest.cpp
    CodecStressTest.cpp
    CryptoStressTest.cpp
    PacketCodecTest.cpp
    # The SRP6 unit realmd calls and the golden vectors captured before the switch.
    Srp6Test.cpp
    CryptoGoldenTest.cpp
    # The in-house primitives (src/shared/Crypto), checked against the vectors above.
    CryptoHashTest.cpp
    CryptoRandomTest.cpp
    CryptoBigIntTest.cpp
    CryptoMontgomeryTest.cpp
    CryptoRsaTest.cpp
    RsaTestKey.h
    CryptoPrimeTest.cpp
    PrimeVectors.h
    CryptoOracleTest.cpp
    CryptoBench.cpp
    BigIntVectors.h
    CryptoGoldenVectors.h
    ${CMAKE_SOURCE_DIR}/src/realmd/Auth/Srp6.cpp
)

# The socket tests. Off unless asked for -- see the note above.
if(WITH_NET_TESTS)
    list(APPEND SRC_GRP_TESTS GracefulCloseTest.cpp)
    list(APPEND SRC_GRP_TESTS NetworkStressTest.cpp)
endif()

source_group("tests" FILES ${SRC_GRP_TESTS})

add_executable(mangos_tests ${SRC_GRP_TESTS})

target_include_directories(mangos_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/game/WorldHandlers
        ${CMAKE_SOURCE_DIR}/src/game/Server
        ${CMAKE_SOURCE_DIR}/src/game/MotionGenerators
        ${CMAKE_SOURCE_DIR}/src/game/Harness
        # The legacy movement layouts (header-only arrays) the wire codec's seed
        # tables are checked against until P1 replaces them with binary goldens.
        ${CMAKE_SOURCE_DIR}/src/game/movement
        ${CMAKE_SOURCE_DIR}/src/realmd
        ${CMAKE_SOURCE_DIR}/src/tools/loadtest/src)

# Where the golden captures live; GoldenCaptureTest replays the ones it names.
target_compile_definitions(mangos_tests PRIVATE MANGOS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens")

target_link_libraries(mangos_tests
    PRIVATE
        motion
        proto
        shared
        mangos_crypto
        extractor_client
        extractor_data
        Threads::Threads
)

# The big-integer oracle (src/tests/oracle): s2n-bignum's proved routines, assembled where
# their GAS sources build -- Linux x86-64, GCC or Clang -- and compared with the in-house
# kernels in CryptoOracleTest. Elsewhere the same comparisons run against the Python vectors.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT MSVC)
    add_subdirectory(oracle)
    target_link_libraries(mangos_tests PRIVATE s2n_bignum_oracle)
    target_compile_definitions(mangos_tests PRIVATE MANGOS_CRYPTO_ORACLE=1)
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

add_test(NAME warden_dormant_boundary
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckDormantWardenBoundary.cmake)

# Nothing of ours names, links or documents the crypto library the tree used to sit on;
# the two greps are exact (see the script).
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

# No future feature may adopt one of the 47 opcode values we know the 4.3.4
# client disagrees with. Checks construction AND dispatch: a wrong CMSG value is
# invisible to a construction-only test, which is how the calendar's request side
# stayed broken through two revisions of its own design doc.
add_test(NAME opcode_denylist
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckOpcodeDenylist.cmake)

# The MotionMaster facade pins the entry points the vendored SD3 scripts call.
# A new call widens the promise and fails the gate; the README carries the inventory.
add_test(NAME motion_shim
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckMotionMasterShim.cmake)

# Unit::Blocked takes the kernel's reason bits (P5-C2 deleted its UnitState overload). An ORed
# mask of unit-state constants is an int and would still bind, read as reasons; the gate refuses
# any Blocked( call that names a UNIT_STAT_ constant (P5-C3).
add_test(NAME motion_blocked_masks
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckBlockedMasks.cmake)

# An update packet's header map id is acted on by the 4.3.4 client: a foreign id is a world
# change that destroys its active player. A passenger's GetMapId() is the vessel's hull map,
# so every sender must build its UpdateData from WorldObject::GetClientMapId().
add_test(NAME client_map_id
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckClientMapId.cmake)

# The console may only stop the world on EOF when stdin is a terminal. A service
# manager passes /dev/null, which is EOF at once; treating that as an operator
# shutdown request stops every headless deployment a second after start-up, and
# exits 0 while doing it. See the script for why this is shape and not behaviour.
add_test(NAME headless_console
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckHeadlessConsole.cmake)

# Put the MySQL client DLL beside the test binary so it can be run from the build tree.
# A convenience, never a gate: the path is derived from the library that was found.
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
