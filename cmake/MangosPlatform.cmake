if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PLATFORM 64)
else()
    set(PLATFORM 32)
endif()

if(XCODE)
    if(PLATFORM EQUAL 32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
        set(CMAKE_OSX_ARCHITECTURES ARM32)
    elseif(PLATFORM EQUAL 32)
        set(CMAKE_OSX_ARCHITECTURES i386)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
        set(CMAKE_OSX_ARCHITECTURES ARM64)
    else()
        set(CMAKE_OSX_ARCHITECTURES x86_64)
    endif()
endif()

if(WIN32)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MSVC)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
endif()

if(MINGW)
    add_compile_definitions(
        WINVER=0x0600
        _WIN32_WINNT=0x0600
    )
    if(PLATFORM EQUAL 32)
        add_compile_definitions(HAVE_SSE2 __SSE2__)
    endif()
endif()

if(MSVC)
    add_compile_options(
        /MP                                     # parallel compilation
        /W4
        /bigobj                                 # section limit, not a debug aid
        $<$<EQUAL:${PLATFORM},32>:/arch:SSE2>
        $<$<CONFIG:Release>:/Gw>                # whole-program global data opt
        $<$<CONFIG:Release>:/GF>                # string pooling

        /wd4018 /wd4100 /wd4101 /wd4127 /wd4131 /wd4189 /wd4244 /wd4245
        /wd4267 /wd4302 /wd4305 /wd4311 /wd4389 /wd4456 /wd4458 /wd4581
        /wd4589 /wd4701 /wd4702 /wd4703 /wd4706 /wd4840 /wd4996
    )

    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>
                     $<$<CONFIG:Release>:/OPT:REF>
                     $<$<CONFIG:Release>:/OPT:ICF>)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Winit-self

        $<$<CONFIG:Release>:-g>

        $<$<CONFIG:Release>:-fno-omit-frame-pointer>
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
        -Winvalid-pch

        -Wno-psabi

        $<$<CONFIG:Debug>:-g3>
    )
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "i386")
        add_compile_options(-msse2 -mfpmath=sse)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
        -Woverloaded-virtual

        -Wno-c++11-narrowing
        -Wno-inconsistent-missing-override
        -Wno-switch
    )
endif()

if(MSVC)
    set(CMAKE_VS_INCLUDE_INSTALL_TO_DEFAULT_BUILD ON)
endif()

include(CheckCXXCompilerFlag)

check_cxx_compiler_flag("-fpch-instantiate-templates" HAVE_FPCH_INSTANTIATE_TEMPLATES)

function(ADD_CXX_PCH TARGET_NAME PRECOMPILED_HEADER)
	if(NOT TARGET ${TARGET_NAME})
		message(FATAL_ERROR "ADD_CXX_PCH: '${TARGET_NAME}' is not a target.")
	endif()

	if(NOT PRECOMPILED_HEADER)
		message(FATAL_ERROR "ADD_CXX_PCH(${TARGET_NAME}): no precompiled header given.")
	endif()

	get_filename_component(_pch "${PRECOMPILED_HEADER}" ABSOLUTE
		BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

	if(NOT EXISTS "${_pch}")
		message(FATAL_ERROR "ADD_CXX_PCH(${TARGET_NAME}): header '${_pch}' does not exist.")
	endif()

	target_precompile_headers(${TARGET_NAME} PRIVATE "${_pch}")

	if(HAVE_FPCH_INSTANTIATE_TEMPLATES)
		target_compile_options(${TARGET_NAME} PRIVATE -fpch-instantiate-templates)
	endif()

	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(${TARGET_NAME} PRIVATE -Xclang -fno-pch-timestamp)
	endif()
endfunction()
