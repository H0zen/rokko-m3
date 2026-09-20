set(CRYPTO_SOURCES
    Fatal.cpp
    Fatal.h
    Hex.cpp
    Hex.h
    BlockHash.h
    Sha1.cpp
    Sha1.h
    Sha256.cpp
    Sha256.h
    Md5.cpp
    Md5.h
    HmacSha1.cpp
    HmacSha1.h
    SecureZero.cpp
    SecureZero.h
    ErasingAllocator.h
    Limb.h
    BigInt.cpp
    BigInt.h
    SystemRandom.cpp
    SystemRandom.h
    Montgomery.cpp
    Montgomery.h
    MontgomeryX86.cpp
    ModExp.cpp
    ModExp.h
    Rsa.cpp
    Rsa.h
    Prime.cpp
    Prime.h
)

set(CRYPTO_ASM_SOURCES)
set(CRYPTO_HAVE_ASM OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    if(MSVC)
        enable_language(ASM_MASM)
        set(CRYPTO_ASM_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/MontgomeryAsm.obj)
        add_custom_command(
            OUTPUT ${CRYPTO_ASM_OBJECT}
            COMMAND ${CMAKE_ASM_MASM_COMPILER} /nologo /c /Fo ${CRYPTO_ASM_OBJECT} ${CMAKE_CURRENT_SOURCE_DIR}/MontgomeryAsm.asm
            MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/MontgomeryAsm.asm
            COMMENT "Assembling the Montgomery kernel (MontgomeryAsm.asm)"
            VERBATIM)
        set_source_files_properties(${CRYPTO_ASM_OBJECT} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        set(CRYPTO_ASM_SOURCES ${CRYPTO_ASM_OBJECT} MontgomeryAsm.asm)
        set_source_files_properties(MontgomeryAsm.asm PROPERTIES HEADER_FILE_ONLY TRUE)
        set(CRYPTO_HAVE_ASM ON)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        enable_language(ASM)
        set(CRYPTO_ASM_SOURCES MontgomeryAsm.S)
        set(CRYPTO_HAVE_ASM ON)
    endif()
endif()

add_library(mangos_crypto STATIC ${CRYPTO_SOURCES} ${CRYPTO_ASM_SOURCES})
if(CRYPTO_HAVE_ASM)
    target_compile_definitions(mangos_crypto PRIVATE MANGOS_CRYPTO_HAVE_ASM=1)
endif()

target_include_directories(mangos_crypto
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src/shared)

target_link_libraries(mangos_crypto PUBLIC $<$<BOOL:${WIN32}>:bcrypt>)

target_compile_features(mangos_crypto PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(mangos_crypto PRIVATE /W4 /permissive-)
else()
    target_compile_options(mangos_crypto PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(mangos_crypto PROPERTIES FOLDER "shared")
