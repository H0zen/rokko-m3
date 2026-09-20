if(NOT DEFINED SOURCE_ROOT)
    get_filename_component(SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
get_filename_component(SOURCE_ROOT "${SOURCE_ROOT}" ABSOLUTE)

set(WORD_PATTERN "openssl|libssl|libcrypto|legacy[.]dll")
set(SYMBOL_PATTERN "(^|[^A-Za-z0-9_])((OSSL|EVP|BN|OPENSSL|SHA1|MD5|HMAC|RAND)_[A-Za-z]|BIGNUM([^A-Za-z0-9_]|$))")

set(OFFENDERS "")

file(GLOB_RECURSE BUILD_FILES
     "${SOURCE_ROOT}/CMakeLists.txt"
     "${SOURCE_ROOT}/cmake/*.cmake"
     "${SOURCE_ROOT}/src/*CMakeLists.txt"
     "${SOURCE_ROOT}/dep/*CMakeLists.txt"
     "${SOURCE_ROOT}/.github/workflows/*.yml")

foreach(FILE_PATH IN LISTS BUILD_FILES)
    if(FILE_PATH MATCHES "CheckCryptoIsOurs")
        continue()
    endif()

    file(STRINGS "${FILE_PATH}" LINES)
    foreach(LINE IN LISTS LINES)
        if(LINE MATCHES "^[ \t]*#")
            continue()
        endif()

        string(TOLOWER "${LINE}" LOWER)
        if(LOWER MATCHES "${WORD_PATTERN}")
            file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${FILE_PATH}")
            list(APPEND OFFENDERS "${SHORT}: ${LINE}")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE SOURCES
     "${SOURCE_ROOT}/src/*.h" "${SOURCE_ROOT}/src/*.hpp"
     "${SOURCE_ROOT}/src/*.c" "${SOURCE_ROOT}/src/*.cpp")

set(SCANNED 0)

foreach(FILE_PATH IN LISTS SOURCES)
    if(FILE_PATH MATCHES "/src/modules/" OR FILE_PATH MATCHES "CheckCryptoIsOurs")
        continue()
    endif()

    math(EXPR SCANNED "${SCANNED} + 1")

    file(READ "${FILE_PATH}" CONTENT)
    if(CONTENT MATCHES "${SYMBOL_PATTERN}")
        string(REGEX MATCH "${SYMBOL_PATTERN}[A-Za-z0-9_]*" HIT "${CONTENT}")
        file(RELATIVE_PATH SHORT "${SOURCE_ROOT}" "${FILE_PATH}")
        list(APPEND OFFENDERS "${SHORT}: ${HIT}")
    endif()
endforeach()

if(SCANNED LESS 100)
    message(FATAL_ERROR "crypto_is_ours: scanned only ${SCANNED} sources under ${SOURCE_ROOT}; the walk is broken")
endif()

if(OFFENDERS)
    list(REMOVE_DUPLICATES OFFENDERS)
    string(REPLACE ";" "\n  " PRETTY "${OFFENDERS}")
    message(FATAL_ERROR
        "crypto_is_ours: the tree still reaches for the removed crypto library:\n  ${PRETTY}\n")
endif()

message(STATUS "crypto_is_ours: ${SCANNED} sources scanned, nothing links or calls the removed library")
