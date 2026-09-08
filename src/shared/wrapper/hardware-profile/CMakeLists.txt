# argus::hardware-profile / argus::hardware-profile-gpu: CPU/RAM/ISA and video
# accel probing from the same source, split so most consumers stay ncnn-free
# (rule 25). Only the Vulkan GPU probe differs between the two targets.

argus_module(NAME hardware-profile
    SOURCES
        hardware-profile.cc
        hardware-profile.hxx
    INCLUDES ../../..)

target_compile_definitions(argus_hardware-profile PRIVATE ARGUS_NO_NCNN_GPU=1)
target_compile_options(argus_hardware-profile PRIVATE -Wall -Wextra)

argus_module(NAME hardware-profile-gpu
    SOURCES
        hardware-profile.cc
        hardware-profile.hxx
    INCLUDES ../../..)

target_compile_options(argus_hardware-profile-gpu PRIVATE -Wall -Wextra)

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_hardware-profile-gpu PUBLIC ${ARGUS_NCNN_TARGET})
endif()
