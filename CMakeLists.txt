cmake_minimum_required(VERSION 3.15)
project(minim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(minim
    src/main.c
    src/alloc.c
)

target_include_directories(minim PRIVATE include)
