cmake_minimum_required(VERSION 3.24)
project(ASTRA VERSION 0.1.0 LANGUAGES C CXX)

# Enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE intellisense (clangd, vscode)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options for Sanitizers
option(ASTRA_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ASTRA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ASTRA_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# Standard warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -pthread
    )
endif()

# Sanitizer configuration
if (ASTRA_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if (ASTRA_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

if (ASTRA_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# Dependencies
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(NCURSES REQUIRED ncurses)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
find_package(nlohmann_json 3.2.0 REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NCURSES_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
)

# Common library dependencies
set(ASTRA_COMMON_LIBS
    Threads::Threads
    nlohmann_json::nlohmann_json
    ${NCURSES_LIBRARIES}
    ${SQLITE3_LIBRARIES}
)

# ASTRA Core Library
add_library(astra_core STATIC
    src/resources/config.cpp
    src/storage/job_store.cpp
    src/cli/command_parser.cpp
    src/cli/cli_handler.cpp
    src/ipc/ipc_channel.cpp
    src/worker/worker.cpp
    src/worker/worker_manager.cpp
)
target_include_directories(astra_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(astra_core PUBLIC ${ASTRA_COMMON_LIBS})

# Workload Binaries
add_executable(cpu_burn workloads/cpu_burn.cpp)
set_target_properties(cpu_burn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/workloads")

add_executable(deliberate_failure workloads/deliberate_failure.cpp)
set_target_properties(deliberate_failure PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/workloads")

add_executable(parallel_sum workloads/parallel_sum.cpp)
target_link_libraries(parallel_sum PRIVATE Threads::Threads)
set_target_properties(parallel_sum PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/workloads")

# Main ASTRA scheduler executable
add_executable(astra
    src/main.cpp
)
target_link_libraries(astra PRIVATE astra_core ${ASTRA_COMMON_LIBS})

# Enable Testing & Unit Tests
enable_testing()

add_executable(test_phase0
    tests/unit/test_phase0.cpp
)
target_link_libraries(test_phase0 PRIVATE astra_core ${ASTRA_COMMON_LIBS})
add_test(NAME Phase0Sanity COMMAND test_phase0)
set_tests_properties(Phase0Sanity PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(test_phase1
    tests/unit/test_phase1.cpp
)
target_link_libraries(test_phase1 PRIVATE astra_core ${ASTRA_COMMON_LIBS})
add_test(NAME Phase1DomainAndStorage COMMAND test_phase1)
set_tests_properties(Phase1DomainAndStorage PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(test_phase2
    tests/unit/test_phase2.cpp
)
target_link_libraries(test_phase2 PRIVATE astra_core ${ASTRA_COMMON_LIBS})
add_test(NAME Phase2CLI COMMAND test_phase2)
set_tests_properties(Phase2CLI PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(test_phase3
    tests/integration/test_worker_lifecycle.cpp
)
target_link_libraries(test_phase3 PRIVATE astra_core ${ASTRA_COMMON_LIBS})
add_dependencies(test_phase3 cpu_burn deliberate_failure parallel_sum)
add_test(NAME Phase3WorkersAndIPC COMMAND test_phase3)
set_tests_properties(Phase3WorkersAndIPC PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
