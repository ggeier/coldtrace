cmake_minimum_required(VERSION 3.20)
project(
  coldtrace
  VERSION 0.1
  LANGUAGES C CXX)

option(COLDTRACE_ENABLE_COVERAGE "Enable Clang coverage instrumentation" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions(LOG_PREFIX="coldtrace: ")
# Platform specific interfaces
set(HAVE_PTHREAD_CLOCKLOCK_CLOCKWAIT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  file(READ "/etc/os-release" _os)
  string(FIND "${_os}" "ID=alpine" _alpine)
  if(_alpine EQUAL -1)
    set(HAVE_PTHREAD_CLOCKLOCK_CLOCKWAIT ON)
  endif()
elseif(NOT CMAKE_SYSTEM_NAME MATCHES "NetBSD" AND NOT APPLE)
  set(HAVE_PTHREAD_CLOCKLOCK_CLOCKWAIT ON)
endif()

add_compile_options(-fPIC -g3 -D_GNU_SOURCE -DDICE_MAIN_START_DISABLE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
if(COLDTRACE_ENABLE_COVERAGE)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()
set(DICE_MEMSET ON)

if(COLDTRACE_ENABLE_COVERAGE)
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "COLDTRACE_ENABLE_COVERAGE requires Clang")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()

add_subdirectory(deps/dice)
find_package(libvsync 4.2 CONFIG)
if(NOT libvsync_DIR)
  set(CMAKE_FIND_ROOT_PATH ${CMAKE_CURRENT_BINARY_DIR}/deps)
  find_package(libvsync 4.2 CONFIG REQUIRED)
endif()
add_library(vsync ALIAS libvsync::vsync)

add_compile_options(
  -Wall
  -Werror
  -Wextra
  -Wno-uninitialized
  -Wno-unused-variable
  -Wno-unused-parameter
  -Wno-unused-function
  -Wno-ignored-qualifiers
  -Wno-frame-address
  -Wno-unused-command-line-argument)
include_directories(include)
add_subdirectory(src)

enable_testing()
add_subdirectory(test)

if(COLDTRACE_ENABLE_COVERAGE)
  find_program(LLVM_PROFDATA_TOOL llvm-profdata REQUIRED)
  find_program(LLVM_COV_TOOL llvm-cov REQUIRED)

  get_property(COLDTRACE_TEST_TARGETS GLOBAL PROPERTY COLDTRACE_TEST_TARGETS)
  set(COVERAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/coverage)
  set(COVERAGE_PROFILE ${COVERAGE_DIR}/coldtrace.profraw)
  set(COVERAGE_DATA ${COVERAGE_DIR}/coldtrace.profdata)

  if(NOT COLDTRACE_TEST_TARGETS)
    message(FATAL_ERROR "No test targets were found for coverage generation")
  endif()
  list(GET COLDTRACE_TEST_TARGETS 0 COVERAGE_MAIN_TARGET)

  set(COVERAGE_OBJECTS)
  foreach(TARGET ${COLDTRACE_TEST_TARGETS})
    list(APPEND COVERAGE_OBJECTS -object=$<TARGET_FILE:${TARGET}>)
  endforeach()
  string(JOIN " " COVERAGE_OBJECTS_STR ${COVERAGE_OBJECTS})

  add_custom_target(
    coverage
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${COVERAGE_PROFILE}
            ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND ${LLVM_PROFDATA_TOOL} merge -sparse ${COVERAGE_PROFILE} -o
            ${COVERAGE_DATA}
    COMMAND ${LLVM_COV_TOOL} report
            $<TARGET_FILE:${COVERAGE_MAIN_TARGET}>
            ${COVERAGE_OBJECTS}
            -instr-profile=${COVERAGE_DATA}
    COMMAND bash -lc "${LLVM_COV_TOOL} export -format=lcov $<TARGET_FILE:${COVERAGE_MAIN_TARGET}> ${COVERAGE_OBJECTS_STR} -instr-profile=${COVERAGE_DATA} > ${COVERAGE_DIR}/coverage.lcov"
    DEPENDS ${COLDTRACE_TEST_TARGETS}
    USES_TERMINAL
    COMMENT "Running tests and producing a Clang coverage report")
endif()

file(GLOB_RECURSE ALL_SOURCE_FILES
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${CMAKE_SOURCE_DIR}/include/*.h
    ${CMAKE_SOURCE_DIR}/test/*.h
    ${CMAKE_SOURCE_DIR}/test/*.cpp
    ${CMAKE_SOURCE_DIR}/test/*.c)

add_custom_target(format
    COMMAND clang-format -style=file -i ${ALL_SOURCE_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-format on all source files")

add_custom_target(diff-check COMMAND git --no-pager diff --exit-code)
