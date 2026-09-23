# Entered only when the game library is built (src/CMakeLists.txt), so the one question left
# is whether the scripts are wanted; the game library links mangosscript whenever they are.
if(SCRIPT_LIB_SD3)
    add_subdirectory(SD3)
endif()
