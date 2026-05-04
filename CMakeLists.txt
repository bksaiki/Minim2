cmake_minimum_required(VERSION 3.15)
project(minim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MINIM_GC_STRESS "Trigger a full GC on every allocation (debug)" OFF)

# Runtime sources, packaged as a static library so each consumer
# (the `minim` binary plus every test target) links against one
# already-compiled archive instead of recompiling the runtime per
# target.
# Bake lib/core.scm into a generated header. configure_file runs at
# CMake-configure time; CMAKE_CONFIGURE_DEPENDS makes editing the .scm
# trigger a re-configure (and thus a re-bake) on the next build.
set_property(DIRECTORY APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS lib/core.scm)
file(READ ${CMAKE_SOURCE_DIR}/lib/core.scm CORE_LIB_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1, " CORE_LIB_BYTES "${CORE_LIB_HEX}")
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
configure_file(
    ${CMAKE_SOURCE_DIR}/src/core_lib_data.h.in
    ${CMAKE_BINARY_DIR}/generated/core_lib_data.h
    @ONLY
)

add_library(minim_rt STATIC
    src/alloc.c
    src/equal.c
    src/eval.c
    src/gc.c
    src/prims.c
    src/read.c
    src/symbol.c
    src/system.c
    src/write.c
)

# `include/` is part of the runtime's public-ish interface — anything
# linking minim_rt needs the headers, so propagate via PUBLIC. The
# generated core-lib header is implementation-private (only
# system.c includes it), so it goes via PRIVATE.
target_include_directories(minim_rt PUBLIC include)
target_include_directories(minim_rt PRIVATE ${CMAKE_BINARY_DIR}/generated)

if(MINIM_GC_STRESS)
    # Stress mode is a runtime-affecting compile-time flag. Make it
    # PUBLIC so dependents inherit it consistently — otherwise the
    # static archive could be built without stress while a test
    # consumer is built with it (or vice versa) and assumptions in
    # inline header code would diverge.
    target_compile_definitions(minim_rt PUBLIC MINIM_GC_STRESS=1)
endif()

# REPL binary.
add_executable(minim src/main.c)
target_link_libraries(minim PRIVATE minim_rt)

enable_testing()
add_subdirectory(tests)
