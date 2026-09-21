# runtime: the process-runtime primitives — off-loop blocking tasks,
# cancellation, the adaptive thread budget, the shared AI-engine mutex — and
# the hardware profile the AI services size their pools from. Consumers link
# argus::lib::runtime, argus::hardware-profile or argus::hardware-profile-gpu.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME runtime
    SOURCES
        src/runtime/thread-budget.cc
        src/runtime/thread-budget.hxx
        src/runtime/ai-init.hxx
        src/runtime/blocking-task.hxx
        src/runtime/cancellation-token.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_lib_runtime PRIVATE -Wall -Wextra)

# The profile probes CPU/RAM/ISA and video accel from the same source, split so
# most consumers stay ncnn-free (rule 25). Only the Vulkan GPU probe differs
# between the two targets.
argus_module(NAME hardware-profile
    SOURCES
        src/runtime/hardware-profile.cc
        src/runtime/hardware-profile.hxx
    INCLUDES src)

target_compile_definitions(argus_hardware-profile PRIVATE ARGUS_NO_NCNN_GPU=1)
target_compile_options(argus_hardware-profile PRIVATE -Wall -Wextra)

argus_module(NAME hardware-profile-gpu
    SOURCES
        src/runtime/hardware-profile.cc
        src/runtime/hardware-profile.hxx
    INCLUDES src)

target_compile_options(argus_hardware-profile-gpu PRIVATE -Wall -Wextra)

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_hardware-profile-gpu PUBLIC ${ARGUS_NCNN_TARGET})
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(thread-budget-test tests/unit/thread-budget-test.cc)
target_link_libraries(thread-budget-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(thread-budget-test PRIVATE -Wall -Wextra)
add_test(NAME thread-budget-test COMMAND thread-budget-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
