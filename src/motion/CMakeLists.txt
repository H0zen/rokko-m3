set(SRC_GRP_MOTION
    Change.h
    Change.cpp
    Authority.h
    Authority.cpp
    PacketMatrix.h
    PacketMatrix.cpp
    PendingChanges.h
    PendingChanges.cpp
    TimeBase.h
    TimeBase.cpp
    Writers.h
    Writers.cpp
    State.h
    State.cpp
    Mobility.h
    Mobility.cpp
    Arbiter.h
    Arbiter.cpp
    MoveIntent.h
    BehaviourModel.h
    SimpleMoves.h
    SimpleMoves.cpp
    DefaultMoves.h
    DefaultMoves.cpp
    PatrolWelding.h
    PatrolWelding.cpp
    TargetKinematics.h
    TargetKinematics.cpp
    TrackingMoves.h
    TrackingMoves.cpp
    ControlMoves.h
    ControlMoves.cpp
    TaxiMove.h
    TaxiMove.cpp
)
source_group("motion" FILES ${SRC_GRP_MOTION})

add_library(motion STATIC ${SRC_GRP_MOTION})

target_include_directories(motion
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(motion
    PUBLIC
        proto
)
