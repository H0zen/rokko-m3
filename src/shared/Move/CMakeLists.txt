add_library(movement STATIC
    Behaviours.cpp
    Behaviours.h
    ClientRules.h
    Crossings.cpp
    Crossings.h
    MoveWriter.cpp
    MoveWriter.h
    Movement.cpp
    Movement.h
    Route.cpp
    Route.h
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
