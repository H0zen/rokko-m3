add_executable(mangos-height-check HeightCheck.cpp)

target_link_libraries(mangos-height-check PRIVATE terrain)

set_target_properties(mangos-height-check PROPERTIES FOLDER "tools")

install(TARGETS mangos-height-check DESTINATION ${BIN_DIR}/tools)
