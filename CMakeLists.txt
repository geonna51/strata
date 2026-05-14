cmake_minimum_required(VERSION 3.15)
project(strata VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Enable Address and UndefinedBehavior sanitizers" OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

if(APPLE)
  execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE MACOS_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(EXISTS "${MACOS_SDK_PATH}/usr/include/c++/v1")
    add_compile_options(-nostdinc++ -isystem ${MACOS_SDK_PATH}/usr/include/c++/v1)
  endif()
endif()

if (ENABLE_SANITIZERS)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
endif()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(STRATA_SOURCES
  src/db_impl.cc
  src/memtable.cc
)

add_library(strata STATIC ${STRATA_SOURCES})

enable_testing()

add_executable(db_test tests/db_test.cc)
target_link_libraries(db_test PRIVATE strata)
add_test(NAME db_test COMMAND db_test)

add_executable(memtable_test tests/memtable_test.cc)
target_link_libraries(memtable_test PRIVATE strata)
add_test(NAME memtable_test COMMAND memtable_test)
