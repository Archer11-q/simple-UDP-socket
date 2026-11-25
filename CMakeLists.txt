cmake_minimum_required(VERSION 3.21)

project(simple_udp_socket LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8 /W4)
endif()

add_executable(udpdemo src/main.cpp)
target_link_libraries(udpdemo PRIVATE ws2_32)
