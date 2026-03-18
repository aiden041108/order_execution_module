cmake_minimum_required(VERSION 3.20)
project(order_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O3 -march=native)
endif()

add_library(order_engine_lib order_engine.cpp)
target_include_directories(order_engine_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(order_engine_lib PUBLIC Threads::Threads)
endif()

add_executable(oe_runner main.cpp)
target_link_libraries(oe_runner PRIVATE order_engine_lib)

add_executable(dummy_runner dummy_runner.cpp)
target_link_libraries(dummy_runner PRIVATE order_engine_lib)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

message(STATUS "Build: ${CMAKE_BUILD_TYPE} | C++${CMAKE_CXX_STANDARD} | ${CMAKE_CXX_COMPILER_ID}")
