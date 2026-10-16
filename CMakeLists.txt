cmake_minimum_required(VERSION 3.20)
project(SurveyDataSDK VERSION 0.2.1 LANGUAGES C CXX)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND MSVC_VERSION LESS 1929)
  message(FATAL_ERROR
    "SurveyDataSDK needs a C++20 compiler. Use VS 2019 16.11 or newer, "
    "or modern MSYS2 GCC/Clang in a separate build directory.")
endif()

option(SURVEY_ENABLE_SANITIZERS
  "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(SURVEY_SANITIZERS "address,undefined"
  CACHE STRING "GCC/Clang sanitizer list")
option(SURVEY_BUILD_TESTS "Build the tests" ON)

add_library(survey_options INTERFACE)
target_compile_features(survey_options INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(survey_options INTERFACE /W4 /WX /permissive-)
else()
  target_compile_options(survey_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

if(SURVEY_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(survey_options INTERFACE
      -fsanitize=${SURVEY_SANITIZERS}
      -fno-omit-frame-pointer)
    target_link_options(survey_options INTERFACE
      -fsanitize=${SURVEY_SANITIZERS})
  else()
    message(FATAL_ERROR
      "This sanitizer configuration requires GCC or Clang")
  endif()
endif()

include(cmake/SQLiteSource.cmake)

add_library(survey_sqlite STATIC
  "${SURVEY_SQLITE_SOURCE_DIR}/sqlite3.c")
target_include_directories(survey_sqlite SYSTEM PUBLIC
  "${SURVEY_SQLITE_SOURCE_DIR}")
set_target_properties(survey_sqlite PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED YES)
target_compile_definitions(survey_sqlite PRIVATE SQLITE_THREADSAFE=1)
target_link_libraries(survey_sqlite PUBLIC ${CMAKE_DL_LIBS})

if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(survey_sqlite PUBLIC Threads::Threads m)
endif()

add_library(survey_domain src/domain.cpp)
target_include_directories(survey_domain PUBLIC include)
target_link_libraries(survey_domain PUBLIC survey_options)

add_library(survey_storage src/sqlite_repository.cpp)
target_link_libraries(survey_storage
  PUBLIC survey_domain
  PRIVATE survey_sqlite)

add_library(survey_json INTERFACE)
target_include_directories(survey_json SYSTEM INTERFACE
  third_party/json/include)

add_library(survey_exchange_core src/exchange.cpp)
target_link_libraries(survey_exchange_core
  PUBLIC survey_domain
  PRIVATE survey_json)

add_executable(survey_demo app/main.cpp)
target_link_libraries(survey_demo PRIVATE survey_storage)

add_executable(survey_exchange app/exchange_main.cpp)
target_link_libraries(survey_exchange PRIVATE
  survey_exchange_core survey_storage)

include(GNUInstallDirs)

install(TARGETS
  survey_domain
  survey_storage
  survey_sqlite
  survey_exchange_core
  survey_demo
  survey_exchange
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES third_party/json/LICENSE.MIT
  DESTINATION share/survey-sdk
  RENAME JSON-LICENSE.MIT)

install(FILES third_party/sources.json
  DESTINATION share/survey-sdk)

if(SURVEY_BUILD_TESTS)
  enable_testing()

  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

  if(MSVC)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  endif()

  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

  add_executable(survey_unit_tests
    tests/domain_unit_tests.cpp
    tests/exchange_json_unit_tests.cpp
    tests/exchange_unit_tests.cpp)
  
  target_link_libraries(survey_unit_tests PRIVATE
    survey_exchange_core
    survey_json
    GTest::gmock_main)

  add_executable(survey_integration_tests
    tests/storage_integration_tests.cpp
    tests/exchange_integration_tests.cpp)

  target_link_libraries(survey_integration_tests PRIVATE
    survey_storage
    survey_sqlite
    survey_exchange_core
    GTest::gtest_main)

  include(GoogleTest)

  gtest_discover_tests(survey_unit_tests
    TEST_PREFIX "unit."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "unit")

  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")

  gtest_discover_tests(survey_integration_tests
    TEST_PREFIX "integration."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS "integration"
      ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-tmp;TEMP=${CMAKE_CURRENT_BINARY_DIR}/test-tmp;TMP=${CMAKE_CURRENT_BINARY_DIR}/test-tmp")

  add_test(NAME survey.roundtrip.processes
    COMMAND ${CMAKE_COMMAND}
      "-DEXCHANGE_TOOL=$<TARGET_FILE:survey_exchange>"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/roundtrip-tests"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")

  set_tests_properties(survey.roundtrip.processes
    PROPERTIES LABELS "integration")
endif()

option(SURVEY_ENABLE_CLANG_TIDY
  "Run clang-tidy on project C++ targets" OFF)

if(SURVEY_ENABLE_CLANG_TIDY)
  if(NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    message(FATAL_ERROR
      "Use a Ninja or Makefiles generator with clang-tidy")
  endif()

  find_program(SURVEY_CLANG_TIDY NAMES clang-tidy REQUIRED)

  set(survey_tidy_targets
    survey_domain
    survey_storage
    survey_exchange_core
    survey_demo
    survey_exchange)

  if(TARGET survey_unit_tests)
    list(APPEND survey_tidy_targets
      survey_unit_tests
      survey_integration_tests)
  endif()

  set_target_properties(${survey_tidy_targets} PROPERTIES
    CXX_CLANG_TIDY "${SURVEY_CLANG_TIDY}")
endif()