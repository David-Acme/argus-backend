include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME runtime
    SOURCES
        src/runtime/thread-budget.cc
        src/runtime/thread-budget.hxx
        src/runtime/shutdown-signal.cc
        src/runtime/shutdown-signal.hxx
        src/runtime/ai-init.hxx
        src/runtime/blocking-task.hxx
        src/runtime/cancellation-token.hxx
        src/runtime/wake-signal.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_lib_runtime PRIVATE -Wall -Wextra)

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

add_executable(shutdown-signal-test tests/unit/shutdown-signal-test.cc)
target_link_libraries(shutdown-signal-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(shutdown-signal-test PRIVATE -Wall -Wextra)
add_test(NAME shutdown-signal-test COMMAND shutdown-signal-test)

add_executable(shutdown-quit-hook-test tests/unit/shutdown-quit-hook-test.cc)
target_link_libraries(shutdown-quit-hook-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(shutdown-quit-hook-test PRIVATE -Wall -Wextra)
add_test(NAME shutdown-quit-hook-test COMMAND shutdown-quit-hook-test)

add_executable(wake-signal-test tests/unit/wake-signal-test.cc)
target_link_libraries(wake-signal-test PRIVATE argus::lib::runtime
    doctest::doctest)
target_compile_options(wake-signal-test PRIVATE -Wall -Wextra)
add_test(NAME wake-signal-test COMMAND wake-signal-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
