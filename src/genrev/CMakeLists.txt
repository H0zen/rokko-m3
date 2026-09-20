if(WITHOUT_GIT OR NOT IS_DIRECTORY "${CMAKE_SOURCE_DIR}/.git")
    return()
endif()

set(REVISION_HEADER "${CMAKE_BINARY_DIR}/src/shared/BuildInfo.h")

set(GIT_STATE_FILES)
foreach(state HEAD index)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.git/${state}")
        list(APPEND GIT_STATE_FILES "${CMAKE_SOURCE_DIR}/.git/${state}")
    endif()
endforeach()

if(NOT GIT_STATE_FILES)
    return()
endif()

add_custom_command(
    OUTPUT "${REVISION_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            -DBUILDDIR=${CMAKE_BINARY_DIR}
            -DCMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -DPROJECT_VERSION=${PROJECT_VERSION}
            -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
            -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
            -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
            -DCONF_INSTALL_DIR=${CONF_INSTALL_DIR}
            -P "${CMAKE_SOURCE_DIR}/cmake/MangosVersion.cmake"
    DEPENDS ${GIT_STATE_FILES}
            "${CMAKE_SOURCE_DIR}/src/shared/BuildInfo.h.in"
            "${CMAKE_SOURCE_DIR}/cmake/MangosVersion.cmake"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Refreshing BuildInfo.h (repository HEAD moved)"
    VERBATIM
)

add_custom_target(revision_data ALL DEPENDS "${REVISION_HEADER}")
