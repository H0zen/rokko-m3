get_filename_component(SOURCE_ROOT "${SOURCE_ROOT}" ABSOLUTE)

set(VERSIONS_FILE "${SOURCE_ROOT}/cmake/MangosVersion.cmake")

if(NOT EXISTS "${VERSIONS_FILE}")
    message(FATAL_ERROR "Version source missing: ${VERSIONS_FILE}")
endif()

set(VIOLATIONS "")

file(GLOB_RECURSE CONF_TEMPLATES "${SOURCE_ROOT}/src/*.conf.dist.in")

foreach(TEMPLATE IN LISTS CONF_TEMPLATES)
    file(STRINGS "${TEMPLATE}" CONF_LINES REGEX "^[ \t]*ConfVersion[ \t]*=")

    if(NOT CONF_LINES)
        file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${TEMPLATE}")
        list(APPEND VIOLATIONS "${SHORT}: no ConfVersion line")
        continue()
    endif()

    foreach(LINE IN LISTS CONF_LINES)
        if(NOT LINE MATCHES "@[A-Za-z0-9_]+@")
            file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${TEMPLATE}")
            list(APPEND VIOLATIONS
                 "${SHORT}: ConfVersion is a literal (${LINE}); use the @MANGOS_..._VER@ the versions file declares")
        endif()
    endforeach()
endforeach()

set(GENERATED_MACROS
    MANGOS_PACKAGENAME
    MANGOS_VERSION_STR
    MANGOSD_CONFIG_VERSION
    REALMD_CONFIG_VERSION
    AHBOT_CONFIG_VERSION
    EXPECTED_MANGOSD_CLIENT_BUILD
    EXPECTED_MANGOSD_CLIENT_VERSION
    PRODUCT_VERSION_RESOURCE
)

file(GLOB_RECURSE SOURCES "${SOURCE_ROOT}/src/*.h" "${SOURCE_ROOT}/src/*.cpp")

foreach(FILE_PATH IN LISTS SOURCES)
    if(FILE_PATH MATCHES "BuildInfo\\.h")
        continue()
    endif()

    foreach(MACRO_NAME IN LISTS GENERATED_MACROS)
        file(STRINGS "${FILE_PATH}" HITS REGEX "^[ \t]*#[ \t]*define[ \t]+${MACRO_NAME}[ \t]")
        if(HITS)
            file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${FILE_PATH}")
            list(APPEND VIOLATIONS
                 "${SHORT}: re-defines ${MACRO_NAME}, which is generated into BuildInfo.h")
        endif()
    endforeach()
endforeach()

file(STRINGS "${SOURCE_ROOT}/src/shared/BuildInfo.h.in" REVISION_LINES
     REGEX "^[ \t]*#[ \t]*define[ \t]+(MANGOS_PACKAGENAME|MANGOS_VERSION_STR|(MANGOSD|REALMD|AHBOT)_CONFIG_VERSION|EXPECTED_MANGOSD_CLIENT|PRODUCT_VERSION_RESOURCE|(REALMD|CHAR|WORLD)_DB_)")

foreach(LINE IN LISTS REVISION_LINES)
    if(NOT LINE MATCHES "@[A-Za-z0-9_]+@")
        list(APPEND VIOLATIONS
             "src/shared/BuildInfo.h.in: literal version (${LINE}); declare it in the versions file")
    endif()
endforeach()

set(BUILDINFO_UMBRELLA "src/shared/Common/Version.cpp")

file(GLOB_RECURSE ALL_SOURCES "${SOURCE_ROOT}/src/*.h" "${SOURCE_ROOT}/src/*.cpp" "${SOURCE_ROOT}/src/*.hpp")

set(INCLUDERS "")

foreach(FILE_PATH IN LISTS ALL_SOURCES)
    if(FILE_PATH MATCHES "BuildInfo\\.h")
        continue()
    endif()

    file(STRINGS "${FILE_PATH}" HITS
         REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<][^\">]*BuildInfo\\.h[\">]")

    if(HITS)
        file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${FILE_PATH}")
        list(APPEND INCLUDERS "${SHORT}")
    endif()
endforeach()

foreach(SHORT IN LISTS INCLUDERS)
    if(NOT SHORT STREQUAL BUILDINFO_UMBRELLA)
        list(APPEND VIOLATIONS
             "${SHORT}: includes BuildInfo.h, so every commit recompiles it; read it through ${BUILDINFO_UMBRELLA}")
    endif()
endforeach()

list(FIND INCLUDERS "${BUILDINFO_UMBRELLA}" UMBRELLA_AT)
if(UMBRELLA_AT EQUAL -1)
    list(APPEND VIOLATIONS "${BUILDINFO_UMBRELLA}: the umbrella no longer includes BuildInfo.h")
endif()

file(GLOB_RECURSE BUILD_FILES "${SOURCE_ROOT}/src/*CMakeLists.txt" "${SOURCE_ROOT}/src/*.cmake")

foreach(FILE_PATH IN LISTS BUILD_FILES)
    file(STRINGS "${FILE_PATH}" HITS REGEX "^[ \t]*set[ \t]*\\([ \t]*MANGOS_")
    if(HITS)
        file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${FILE_PATH}")
        list(APPEND VIOLATIONS
             "${SHORT}: declares a MANGOS_* value of its own (${HITS}); the versions file is where those live")
    endif()
endforeach()

file(GLOB_RECURSE STRAY_RC "${SOURCE_ROOT}/src/*.rc")

foreach(FILE_PATH IN LISTS STRAY_RC)
    file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${FILE_PATH}")
    list(APPEND VIOLATIONS
         "${SHORT}: a second Windows version resource; every executable compiles cmake/win/VersionInfo.rc")
endforeach()

if(VIOLATIONS)
    list(REMOVE_DUPLICATES VIOLATIONS)
    string(REPLACE ";" "\n  " PRETTY "${VIOLATIONS}")
    message(FATAL_ERROR
        "Version declarations have drifted from cmake/MangosVersion.cmake:\n  ${PRETTY}\n")
endif()

message(STATUS "Version sources: single-sourced from cmake/MangosVersion.cmake")
