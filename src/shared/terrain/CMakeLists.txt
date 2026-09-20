add_library(terrain STATIC
    Accelerators.cpp
    Accelerators.hpp
    CollisionModel.cpp
    CollisionModel.hpp
    Column.hpp
    FusedTerrain.cpp
    FusedTerrain.hpp
    Geometry.hpp
    GoModelStore.cpp
    GoModelStore.hpp
    ICollisionModel.hpp
    ILiveGeometry.hpp
    ModelTileSource.hpp
    Terrain.hpp
    TileSerializer.cpp
    TileSerializer.hpp
    WmoModel.cpp
    WmoModel.hpp
)

target_include_directories(terrain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(terrain
    PUBLIC
        geometry
        Threads::Threads
)
