add_library(broclip_api STATIC
    api.cpp
    native_clip.cpp
)

target_include_directories(broclip_api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
)

target_link_libraries(broclip_api PUBLIC
    broclip
    bronze_runtime_shared
)

target_compile_features(broclip_api PUBLIC cxx_std_20)

broclip_warnings(broclip_api)
