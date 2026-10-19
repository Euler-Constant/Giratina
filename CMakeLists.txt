cmake_minimum_required(VERSION 3.16)
project(linscan CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SANITIZE "Build with AddressSanitizer and UBSan" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(linscan_core STATIC
  src/ir.cpp
  src/parser.cpp
  src/cfg.cpp
  src/liveness.cpp
  src/intervals.cpp
  src/vm.cpp
  src/validate.cpp
  src/harness.cpp
  src/alloc_usage.cpp
  src/alloc_linear.cpp
  src/alloc_color.cpp
)
target_include_directories(linscan_core PUBLIC src)

add_executable(linscan src/main.cpp)
target_link_libraries(linscan PRIVATE linscan_core)

enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE linscan_core)
add_test(NAME validator COMMAND test_core validator)
add_test(NAME fig2_linear_scan COMMAND test_core fig2)

add_executable(test_harness tests/test_harness.cpp)
target_link_libraries(test_harness PRIVATE linscan_core)
target_compile_definitions(test_harness PRIVATE LINSCAN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME harness COMMAND test_harness)


# Define a custom build/profiling target for Callgrind
find_program(VALGRIND_BIN valgrind)

if(VALGRIND_BIN)
    add_custom_target(profile
        COMMAND ${VALGRIND_BIN} --tool=callgrind --callgrind-out-file=callgrind.out $<TARGET_FILE:linscan>
        DEPENDS linscan
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running Callgrind profiling on linscan..."
    )
endif()
