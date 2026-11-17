cmake_minimum_required(VERSION 3.20)
project(RedisLite VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Debug or Release" FORCE)
endif()

option(REDISLITE_BUILD_TESTS "Build the GoogleTest suite" ON)

find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(redislite_core STATIC
    src/database.cpp
    src/parser.cpp
    src/executor.cpp
    src/socket.cpp
    src/server.cpp
    src/client.cpp
)
target_include_directories(redislite_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(redislite_core PUBLIC Threads::Threads)

add_executable(redislite src/main.cpp)
target_link_libraries(redislite PRIVATE redislite_core)

add_executable(redislite_benchmark tools/benchmark.cpp)
target_link_libraries(redislite_benchmark PRIVATE redislite_core)

if(REDISLITE_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(redislite_tests
        tests/parser_test.cpp
        tests/executor_test.cpp
        tests/database_test.cpp
        tests/server_test.cpp
    )
    target_link_libraries(redislite_tests PRIVATE redislite_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(redislite_tests PROPERTIES TIMEOUT 30)
endif()
