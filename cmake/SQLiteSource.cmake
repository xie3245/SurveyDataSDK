set(SURVEY_SQLITE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3")

# A compact offline distribution carries the same upstream sources in this archive.
if(NOT EXISTS "${SURVEY_SQLITE_SOURCE_DIR}/sqlite3.c" OR
   NOT EXISTS "${SURVEY_SQLITE_SOURCE_DIR}/sqlite3.h")
  set(archive "${SURVEY_SQLITE_SOURCE_DIR}/sqlite-source.tar.xz")
  if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "SQLite source archive is missing; extract the entire project ZIP")
  endif()
  file(SHA256 "${archive}" archive_hash)
  if(NOT archive_hash STREQUAL "feea50aae5908145aaf00e3584a71eb2574b612e1f1221d207bbfe73df268996")
    message(FATAL_ERROR "SQLite source archive is incomplete or corrupted")
  endif()
  set(SURVEY_SQLITE_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/sqlite-source")
  file(MAKE_DIRECTORY "${SURVEY_SQLITE_SOURCE_DIR}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${SURVEY_SQLITE_SOURCE_DIR}")
  file(TOUCH "${SURVEY_SQLITE_SOURCE_DIR}/sqlite3.c" "${SURVEY_SQLITE_SOURCE_DIR}/sqlite3.h")
  message(STATUS "Unpacked included SQLite sources; no download required")
endif()
