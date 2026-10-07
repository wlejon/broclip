cmake_minimum_required(VERSION 3.24)

project(broclip VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_broclip_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_broclip_top_level ON)
endif()

option(BROCLIP_BUILD_TESTS "Build broclip tests" ${_broclip_top_level})

# MSVC runtime: the default, the DLL CRT (/MD, /MDd), as bro builds with.
if(MSVC AND _broclip_top_level)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(_broclip_top_level AND CMAKE_GENERATOR MATCHES "Ninja")
    if(NOT DEFINED BRO_COMPILE_JOBS)
        set(BRO_COMPILE_JOBS 4)
    endif()
    if(NOT DEFINED BRO_LINK_JOBS)
        set(BRO_LINK_JOBS 2)
    endif()
    set_property(GLOBAL PROPERTY JOB_POOLS
        compile_pool=${BRO_COMPILE_JOBS}
        link_pool=${BRO_LINK_JOBS})
    set(CMAKE_JOB_POOL_COMPILE compile_pool)
    set(CMAKE_JOB_POOL_LINK link_pool)
endif()

function(broclip_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

find_package(Threads REQUIRED)

set(BROCLIP_SOURCES
    src/types.cpp
    src/mime_stream.cpp
    src/ring_store.cpp
    src/clip.cpp
    src/backend.cpp
)

set(BROCLIP_EXTRA_LIBS Threads::Threads)
set(BROCLIP_EXTRA_OBJECTS)
set(BROCLIP_PRIVATE_INCLUDES)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

    set(PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${PROTO_DIR})

    set(BROCLIP_PROTOCOLS
        ext-data-control-v1
        wlr-data-control-unstable-v1
    )

    set(BROCLIP_PROTO_SOURCES)
    set(BROCLIP_PROTO_HEADERS)
    foreach(_proto ${BROCLIP_PROTOCOLS})
        set(_xml ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${_proto}.xml)
        set(_header ${PROTO_DIR}/${_proto}-client-protocol.h)
        set(_code ${PROTO_DIR}/${_proto}-protocol.c)
        add_custom_command(
            OUTPUT ${_header} ${_code}
            COMMAND ${WAYLAND_SCANNER} client-header ${_xml} ${_header}
            COMMAND ${WAYLAND_SCANNER} private-code ${_xml} ${_code}
            DEPENDS ${_xml}
            COMMENT "Generating Wayland client protocol: ${_proto}"
            VERBATIM
        )
        list(APPEND BROCLIP_PROTO_HEADERS ${_header})
        list(APPEND BROCLIP_PROTO_SOURCES ${_code})
    endforeach()

    add_library(broclip_protocols OBJECT ${BROCLIP_PROTO_SOURCES} ${BROCLIP_PROTO_HEADERS})
    target_link_libraries(broclip_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)
    set_target_properties(broclip_protocols PROPERTIES POSITION_INDEPENDENT_CODE ON)

    list(APPEND BROCLIP_EXTRA_OBJECTS $<TARGET_OBJECTS:broclip_protocols>)
    list(APPEND BROCLIP_EXTRA_LIBS PkgConfig::WAYLAND_CLIENT)
    list(APPEND BROCLIP_PRIVATE_INCLUDES ${PROTO_DIR})

    list(APPEND BROCLIP_SOURCES
        src/linux/data_control.cpp
        src/win/clipboard.cpp
        src/mac/pasteboard_stub.cpp
    )
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND BROCLIP_SOURCES
        src/mac/pasteboard.mm
        src/win/clipboard.cpp
    )
    find_library(APPKIT_FRAMEWORK AppKit)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    list(APPEND BROCLIP_EXTRA_LIBS ${APPKIT_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
elseif(WIN32)
    list(APPEND BROCLIP_SOURCES
        src/win/clipboard.cpp
        src/mac/pasteboard_stub.cpp
    )
else()
    list(APPEND BROCLIP_SOURCES
        src/win/clipboard.cpp
        src/mac/pasteboard_stub.cpp
    )
endif()

add_library(broclip STATIC ${BROCLIP_SOURCES} ${BROCLIP_EXTRA_OBJECTS})
add_library(broclip::broclip ALIAS broclip)

if(TARGET broclip_protocols)
    add_dependencies(broclip broclip_protocols)
endif()

target_include_directories(broclip
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${BROCLIP_PRIVATE_INCLUDES}
)

broclip_warnings(broclip)
target_link_libraries(broclip PUBLIC ${BROCLIP_EXTRA_LIBS})

# --- Standalone Bronze JavaScript API ---
# On by default only when broclip is the top-level project. A library that
# consumes broclip then never needs bronze checked out; bro, which does want
# the binding, sets BROCLIP_ENABLE_API ON itself.
option(BROCLIP_ENABLE_API "Build Bronze JavaScript API" ${PROJECT_IS_TOP_LEVEL})
if(BROCLIP_ENABLE_API)
    if(NOT TARGET bronze_runtime_shared)
        set(BRONZE_BUILD_SHARED_RUNTIME ON CACHE BOOL "" FORCE)
        set(BRONZE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        if(DEFINED BRONZE_DIR AND EXISTS "${BRONZE_DIR}/CMakeLists.txt")
            # Explicit BRONZE_DIR supplied
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../bronze/CMakeLists.txt")
            set(BRONZE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bronze" CACHE PATH "bronze source tree")
        elseif(EXISTS "${CMAKE_SOURCE_DIR}/../bronze/CMakeLists.txt")
            set(BRONZE_DIR "${CMAKE_SOURCE_DIR}/../bronze" CACHE PATH "bronze source tree")
        endif()

        if(NOT BRONZE_DIR OR NOT EXISTS "${BRONZE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "bronze not found: check it out beside this repository (../bronze, with ../brass beside it) "
                "or pass -DBRONZE_DIR=<path>.")
        endif()

        add_subdirectory("${BRONZE_DIR}" "${CMAKE_BINARY_DIR}/bronze-build" EXCLUDE_FROM_ALL)
    endif()

    add_subdirectory(src/api)
endif()

if(BROCLIP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
