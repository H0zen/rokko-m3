set(MySQL_FOUND 0)

set(MYSQL_ROOT "$ENV{MYSQL_ROOT}" CACHE PATH "Root of a MySQL or MariaDB installation")
set(_MySQL_hints)
foreach (_MySQL_root IN ITEMS "${MYSQL_ROOT}" "$ENV{MARIADB_ROOT}")
  if (_MySQL_root)
    file(TO_CMAKE_PATH "${_MySQL_root}" _MySQL_root)
    list(APPEND _MySQL_hints "${_MySQL_root}")
  endif ()
endforeach ()
unset(_MySQL_root)

set(_MYSQL_USE_PKGCONFIG 0)
if (NOT WIN32 AND NOT _MySQL_hints)
  find_package(PkgConfig)
  if (PkgConfig_FOUND)
    set(_MYSQL_USE_PKGCONFIG 1)
  endif ()
endif ()

if (_MYSQL_USE_PKGCONFIG)
  pkg_check_modules(_mariadb "mariadb" QUIET IMPORTED_TARGET)
  unset(_mysql_target)
  if (NOT _mariadb_FOUND)
    pkg_check_modules(_mysql "mysql" QUIET IMPORTED_TARGET)
    if (_mysql_FOUND)
      set(_mysql_target "_mysql")
    endif ()
  else ()
    set(_mysql_target "_mariadb")
    if (_mariadb_VERSION VERSION_LESS 10.4)
      get_property(_include_dirs
        TARGET    "PkgConfig::_mariadb"
        PROPERTY  "INTERFACE_INCLUDE_DIRECTORIES")
      list(FILTER _include_dirs EXCLUDE REGEX "\\.\\.")
      set_property(TARGET "PkgConfig::_mariadb"
        PROPERTY
          "INTERFACE_INCLUDE_DIRECTORIES" "${_include_dirs}")
      unset(_include_dirs)
    endif ()
  endif ()
  if (_mysql_target)
    set(MySQL_FOUND 1)
    add_library(MySQL::MySQL INTERFACE IMPORTED)
    target_link_libraries(MySQL::MySQL
      INTERFACE "PkgConfig::${_mysql_target}")
    set(MySQL_INCLUDE_DIRS ${${_mysql_target}_INCLUDE_DIRS})
    set(MySQL_LIBRARIES ${${_mysql_target}_LINK_LIBRARIES})
  endif ()
  unset(_mysql_target)
endif ()

if(NOT MySQL_FOUND)
  set(_MySQL_paths)

  file(GLOB _MySQL_install_dirs
    "C:/Program Files/MariaDB */"
    "C:/Program Files (x86)/MariaDB */"
    "C:/Program Files/MySQL/MySQL Server */"
    "C:/Program Files (x86)/MySQL/MySQL Server */")
  list(APPEND _MySQL_paths ${_MySQL_install_dirs})
  unset(_MySQL_install_dirs)

  find_path(MySQL_INCLUDE_DIR
    NAMES mysql.h
    HINTS ${_MySQL_hints}
    PATHS
      "C:/Program Files/MySQL/include"
      "C:/MySQL/include"
      ${_MySQL_paths}
      /usr
      /usr/include
    PATH_SUFFIXES include include/mysql include/mariadb
    DOC "Location of mysql.h")
  mark_as_advanced(MySQL_INCLUDE_DIR)
  find_library(MySQL_LIBRARY
    NAMES libmariadb mysql libmysql mysqlclient
    HINTS ${_MySQL_hints}
    PATHS
      "C:/Program Files/MySQL/lib"
      "C:/MySQL/lib/debug"
      ${_MySQL_paths}
      /usr
      /usr/local/
    PATH_SUFFIXES lib lib/opt lib/mysql lib/mariadb
    DOC "Location of the mysql library")
  mark_as_advanced(MySQL_LIBRARY)

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(MySQL
    REQUIRED_VARS MySQL_INCLUDE_DIR MySQL_LIBRARY)

  if (MySQL_FOUND)
    add_library(MySQL::MySQL UNKNOWN IMPORTED)
    set_target_properties(MySQL::MySQL PROPERTIES
      IMPORTED_LOCATION "${MySQL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${MySQL_INCLUDE_DIR}")
    set(MySQL_INCLUDE_DIRS "${MySQL_INCLUDE_DIR}")
    set(MySQL_LIBRARIES "${MySQL_LIBRARY}")
  endif ()
endif ()
unset(_MySQL_hints)
unset(_MYSQL_USE_PKGCONFIG)
