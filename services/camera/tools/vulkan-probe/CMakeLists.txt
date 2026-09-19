# Vulkan-in-container RADV probe (argus-vulkan-probe): reuses ncnn's own
# Vulkan init, so it only builds where the ncnn target exists. Built on
# demand, never as part of the default backend build.

if(NOT TARGET ncnn)
    message(FATAL_ERROR "argus-vulkan-probe needs the ncnn target")
endif()

add_executable(argus-vulkan-probe EXCLUDE_FROM_ALL vulkan-probe.cc)

target_include_directories(argus-vulkan-probe SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/ncnn/src)

target_link_libraries(argus-vulkan-probe PRIVATE ${ARGUS_NCNN_TARGET})

target_compile_options(argus-vulkan-probe PRIVATE -Wall -Wextra)
