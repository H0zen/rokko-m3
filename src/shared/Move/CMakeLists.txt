add_library(movement STATIC
    Aim.cpp
    Aim.h
    ClientRules.h
    Crossings.cpp
    Crossings.h
    Leg.cpp
    Leg.h
    Schedule.cpp
    Schedule.h
    Selection.cpp
    Selection.h
)

target_include_directories(movement
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(movement
    PUBLIC
        geometry
)
