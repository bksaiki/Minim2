function(minim_add_test name)
    set(abs_runtime_sources "")
    foreach(src ${MINIM_RUNTIME_SOURCES})
        list(APPEND abs_runtime_sources "${PROJECT_SOURCE_DIR}/${src}")
    endforeach()

    add_executable(${name} ${name}.c ${abs_runtime_sources})
    target_include_directories(${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    if(MINIM_GC_STRESS)
        target_compile_definitions(${name} PRIVATE MINIM_GC_STRESS=1)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

minim_add_test(test_tags)
minim_add_test(test_alloc)
minim_add_test(test_gc)
minim_add_test(test_parser)
minim_add_test(test_writer)
minim_add_test(test_eval)
