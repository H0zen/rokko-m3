add_library(geometry INTERFACE)

target_sources(geometry
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Frame.h
        ${CMAKE_CURRENT_SOURCE_DIR}/GeometryMath.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Matrix4.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Placement.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Quat.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Shapes.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Vector2.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Vector3.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Vector4.h
)

target_include_directories(geometry
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)
