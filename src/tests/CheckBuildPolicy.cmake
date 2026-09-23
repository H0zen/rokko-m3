file(READ "${SOURCE_ROOT}/src/CMakeLists.txt" SRC_CMAKE)
file(READ "${SOURCE_ROOT}/.github/workflows/core_linux_build.yml" LINUX_CI)
file(READ "${SOURCE_ROOT}/.github/workflows/core_windows_build.yml" WINDOWS_CI)

string(FIND "${SRC_CMAKE}" "Upstream realmd passes the *file*" POSITION)
if(NOT POSITION EQUAL -1)
  message(FATAL_ERROR "Obsolete external realmd VersionInfo workaround remains")
endif()

foreach(CI_TEXT IN ITEMS "${LINUX_CI}" "${WINDOWS_CI}")
  foreach(REQUIRED_TEXT "-DWITH_TESTS=1" "ctest --test-dir")
    string(FIND "${CI_TEXT}" "${REQUIRED_TEXT}" POSITION)
    if(POSITION EQUAL -1)
      message(FATAL_ERROR "CI does not run tests: ${REQUIRED_TEXT}")
    endif()
  endforeach()
endforeach()

# Decoupling D2 (server #120): one Linux job builds without the precompiled header, because
# pchdef.h hands every translation unit the whole world and only a PCH-off build proves that a
# file includes what it uses. The workflow must keep a matrix entry with `pch: 0` and pass that
# value to cmake. YAML comment lines are dropped first, so a flag left behind in a comment
# cannot satisfy this check.
set(LINUX_CI_CODE "")
string(REPLACE ";" "\\;" LINUX_CI_ESCAPED "${LINUX_CI}")
string(REPLACE "\n" ";" LINUX_CI_LINES "${LINUX_CI_ESCAPED}")
foreach(LINE IN LISTS LINUX_CI_LINES)
  if(NOT LINE MATCHES "^[ \t]*#")
    string(APPEND LINUX_CI_CODE "${LINE}\n")
  endif()
endforeach()
foreach(REQUIRED_TEXT "pch: 0" "-DPCH=\${{ matrix.pch }}")
  string(FIND "${LINUX_CI_CODE}" "${REQUIRED_TEXT}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "Linux CI no longer builds without the precompiled header (decoupling D2, server #120): missing ${REQUIRED_TEXT}")
  endif()
endforeach()
