cmake_minimum_required(VERSION 3.20)
project(wizroot CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(wizroot
    src/main.cpp
    src/mft.cpp
    src/tree.cpp
    src/snapshot.cpp
)

if(MSVC)
    target_compile_options(wizroot PRIVATE /W4 /utf-8)
endif()
