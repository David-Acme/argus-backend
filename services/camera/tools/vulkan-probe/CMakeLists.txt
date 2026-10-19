if(NOT TARGET ncnn)
    message(FATAL_ERROR "argus-vulkan-probe needs the ncnn target")
endif()

add_executable(argus-vulkan-probe EXCLUDE_FROM_ALL vulkan-probe.cc)

target_link_libraries(argus-vulkan-probe PRIVATE ${ARGUS_NCNN_TARGET})

target_compile_options(argus-vulkan-probe PRIVATE -Wall -Wextra)
