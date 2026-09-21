add_executable(mangos-column-audit ColumnAudit.cpp)

target_link_libraries(mangos-column-audit PRIVATE terrain)

set_target_properties(mangos-column-audit PROPERTIES FOLDER "tools")

install(TARGETS mangos-column-audit DESTINATION ${BIN_DIR}/tools)
