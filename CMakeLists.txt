cmake_minimum_required(VERSION 3.15)
project(minim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MINIM_GC_STRESS "Trigger a full GC on every allocation (debug)" OFF)

set(MINIM_RUNTIME_SOURCES
    src/alloc.c
    src/gc.c
    src/parser.c
    src/symbol.c
    src/system.c
    src/writer.c
)

add_executable(minim
    src/main.c
    ${MINIM_RUNTIME_SOURCES}
)

target_include_directories(minim PRIVATE include)

if(MINIM_GC_STRESS)
    target_compile_definitions(minim PRIVATE MINIM_GC_STRESS=1)
endif()

enable_testing()
add_subdirectory(tests)
