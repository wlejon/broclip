function(broclip_test name)
    cmake_parse_arguments(T "" "TIMEOUT" "LIBS" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE broclip::broclip ${T_LIBS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    broclip_warnings(${name})
    if(NOT T_TIMEOUT)
        set(T_TIMEOUT 60)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT} SKIP_RETURN_CODE 77)
endfunction()

broclip_test(test_clip_types)
broclip_test(test_ring_store)
broclip_test(test_clipboard_manager)
# MIME streams move selection data over file-descriptor pipes, the way the
# Wayland data-control protocol hands it over; Windows has no such transfer.
if(NOT WIN32)
    broclip_test(test_mime_stream)
    broclip_test(test_data_control)
endif()

if(BROCLIP_ENABLE_API)
    add_executable(broclip_test_api
        test_api.cpp
    )
    target_include_directories(broclip_test_api PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/api
        ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(broclip_test_api PRIVATE
        broclip_api
        broclip
        bronze_runtime_shared
    )
    broclip_warnings(broclip_test_api)
    if(WIN32)
        target_compile_definitions(broclip_test_api PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        add_custom_command(TARGET broclip_test_api POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broclip_test_api>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broclip_test_api>
        )
    endif()

    if(NOT BRASS_HOST_BACKEND)
        message(STATUS "broclip: broclip_test_api not registered (brass has no code generator for this target)")
    else()
        add_test(NAME broclip_test_api COMMAND broclip_test_api)
        set_tests_properties(broclip_test_api PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

        add_test(NAME broclip_test_api_gc_stress COMMAND broclip_test_api)
        set_tests_properties(broclip_test_api_gc_stress PROPERTIES
            TIMEOUT 600
            SKIP_RETURN_CODE 77
            ENVIRONMENT "BRONZE_GC_STRESS=1;BRONZE_GC_POISON=1"
        )
        if(WIN32)
            set_tests_properties(broclip_test_api PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broclip_test_api>;$ENV{PATH}")
            set_property(TEST broclip_test_api_gc_stress APPEND PROPERTY
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broclip_test_api>;$ENV{PATH}")
        endif()
    endif()
endif()

# Every test executable links no_dialogs.cpp (see the file).
if(WIN32)
    get_property(_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _test_targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            target_sources(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/no_dialogs.cpp)
        endif()
    endforeach()
endif()
